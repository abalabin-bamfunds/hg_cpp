#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>

#include <bg/scalar/buffer_operations.hpp>
#include <bg/time_series/queue_operations.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/util/scope_exit.hpp>

#include "literal_helpers.hpp"
#include "queue_operations.hpp"
#include "tick_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace bg::frame::details {
namespace {

enum class FrameQueueOperation : std::uint8_t { upsert, remove };

struct FrameQueueChange {
    FrameQueueOperation operation{};
    std::vector<FrameLiteralCell> cells;
    std::vector<std::uint8_t> supplied;
};

void read_frame_cell(util::buffer_reader &reader, const ScalarType &type, FrameLiteralCell &cell) {
    const auto decoded = tick_codec::read_frame_cell(reader, type);
    std::visit([&](const auto &value) {
        using Cell = std::remove_cvref_t<decltype(value)>;
        if constexpr (!std::same_as<Cell, std::monostate>)
            cell = value;
    }, decoded);
}

void project_frame_cell(const FrameLiteralCell &cell, const ScalarType &type,
                        ScalarBufferProjectionContext &projection) {
    visit_cell_type(type, [&]<typename Cell>(std::type_identity<Cell>) {
        tick_codec::project_cell<Cell>(std::get<std::optional<cell_value_t<Cell>>>(cell), projection);
    });
}

void write_frame_cell(const FrameLiteralCell &cell, const ScalarType &type, ScalarBufferWriteContext &writer,
                      allocator &memory) {
    visit_cell_type(type, [&]<typename Cell>(std::type_identity<Cell>) {
        tick_codec::write_cell<Cell>(std::get<std::optional<cell_value_t<Cell>>>(cell), writer, memory);
    });
}

void write_raw(ScalarBufferWriteContext &writer, const auto &value) {
    writer.write_bytes(std::as_bytes(std::span{std::addressof(value), std::size_t{1}}));
}

void project_raw(ScalarBufferProjectionContext &projection, const auto &value) {
    projection.write_bytes(sizeof(value));
}

bool is_key_field(const NativeFrameLayout &layout, std::size_t index) {
    return std::ranges::find(layout.key_fields, index) != layout.key_fields.end();
}

std::span<const literal::ScalarLiteral> frame_queue_literal_entries(const literal::TimeSeriesLiteral &literal) {
    return literal.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
            throw std::invalid_argument{"TSF queue literal must contain a sequence of row changes"};
        },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();
}

FrameQueueChange parse_frame_queue_change(const literal::ScalarLiteral &entry, const NativeFrameLayout &layout) {
    const auto tagged = literal::TaggedLiteral::from(entry);
    const auto removal = tagged.has_value();
    if (removal && tagged->sentinel != literal::REMOVE)
        throw std::invalid_argument{"TSF queue literal only accepts REMOVE row changes"};
    if (removal && layout.key_fields.empty())
        throw std::invalid_argument{"cannot remove an unkeyed TSF row from a queue"};

    const auto fields = read_named_frame_fields(removal ? tagged->argument : entry, *layout.schema, layout, removal);
    auto change = FrameQueueChange{removal ? FrameQueueOperation::remove : FrameQueueOperation::upsert, {},
                                   std::vector<std::uint8_t>(layout.fields.size())};
    initialize_frame_literal_cells(layout, change.cells);
    for (auto index = std::size_t{}; index != fields.size(); ++index) {
        if (fields[index] == nullptr)
            continue;
        read_frame_literal_cell(*fields[index], *layout.fields[index].cell_type, change.cells[index]);
        change.supplied[index] = 1;
    }
    return change;
}

std::span<const literal::ScalarLiteral> column_queue_literal_values(const literal::TimeSeriesLiteral &literal) {
    return literal.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
            throw std::invalid_argument{"TSC queue literal must contain a sequence of cells"};
        },
        [](std::span<const literal::ScalarLiteral> items) { return items; }).value();
}

void project_frame_queue_literal(const literal::TimeSeriesLiteral &literal, const NativeFrameLayout &layout,
                                 ScalarBufferProjectionContext &projection) {
    const auto entries = frame_queue_literal_entries(literal);
    if (entries.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error{"TSF queue literal has too many row changes"};
    const auto count = static_cast<std::uint32_t>(entries.size());
    project_raw(projection, count);
    for (const auto &entry : entries) {
        const auto change = parse_frame_queue_change(entry, layout);
        project_raw(projection, static_cast<std::uint8_t>(change.operation));
        const auto field_count = static_cast<std::uint32_t>(std::ranges::count(change.supplied, std::uint8_t{1}));
        project_raw(projection, field_count);
        for (auto index = std::size_t{}; index != change.supplied.size(); ++index) {
            if (!change.supplied[index])
                continue;
            const auto field_index = static_cast<std::uint32_t>(index);
            project_raw(projection, field_index);
            project_frame_cell(change.cells[index], *layout.fields[index].cell_type, projection);
        }
    }
}

void write_frame_queue_literal(const literal::TimeSeriesLiteral &literal, const NativeFrameLayout &layout,
                               ScalarBufferWriteContext &writer, allocator &memory) {
    const auto entries = frame_queue_literal_entries(literal);
    if (entries.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error{"TSF queue literal has too many row changes"};
    const auto count = static_cast<std::uint32_t>(entries.size());
    write_raw(writer, count);
    for (const auto &entry : entries) {
        const auto change = parse_frame_queue_change(entry, layout);
        const auto operation = static_cast<std::uint8_t>(change.operation);
        write_raw(writer, operation);
        const auto field_count = static_cast<std::uint32_t>(std::ranges::count(change.supplied, std::uint8_t{1}));
        write_raw(writer, field_count);
        for (auto index = std::size_t{}; index != change.supplied.size(); ++index) {
            if (!change.supplied[index])
                continue;
            const auto field_index = static_cast<std::uint32_t>(index);
            write_raw(writer, field_index);
            write_frame_cell(change.cells[index], *layout.fields[index].cell_type, writer, memory);
        }
    }
}

FrameQueueChange read_frame_queue_change(util::buffer_reader &reader, const NativeFrameLayout &layout) {
    const auto operation = static_cast<FrameQueueOperation>(reader.read_raw<std::uint8_t>());
    const auto field_count = reader.read_raw<std::uint32_t>();
    auto change = FrameQueueChange{operation, {}, std::vector<std::uint8_t>(layout.fields.size())};
    initialize_frame_literal_cells(layout, change.cells);
    for (auto field_position = std::uint32_t{}; field_position != field_count; ++field_position) {
        const auto index = reader.read_raw<std::uint32_t>();
        read_frame_cell(reader, *layout.fields[index].cell_type, change.cells[index]);
        change.supplied[index] = 1;
    }
    return change;
}

void discard_frame_queue_payload(util::buffer_reader &reader, const NativeFrameLayout &layout) {
    const auto count = reader.read_raw<std::uint32_t>();
    for (auto position = std::uint32_t{}; position != count; ++position) {
        static_cast<void>(reader.read_raw<std::uint8_t>());
        const auto field_count = reader.read_raw<std::uint32_t>();
        for (auto field_position = std::uint32_t{}; field_position != field_count; ++field_position) {
            const auto index = reader.read_raw<std::uint32_t>();
            tick_codec::discard_cell(reader, *layout.fields[index].cell_type);
        }
    }
}

std::vector<const void *> frame_cell_pointers(const FrameQueueChange &change) {
    auto values = std::vector<const void *>{};
    values.reserve(change.cells.size());
    for (const auto &cell : change.cells)
        values.push_back(frame_literal_cell_pointer(cell));
    return values;
}

std::vector<NativeFrameCellPatch> frame_patches(const FrameQueueChange &change, const NativeFrameLayout &layout) {
    auto patches = std::vector<NativeFrameCellPatch>(change.cells.size());
    for (auto index = std::size_t{}; index != change.cells.size(); ++index)
        if (change.supplied[index] && !is_key_field(layout, index))
            patches[index] = {true, frame_literal_cell_pointer(change.cells[index])};
    return patches;
}

bool has_non_key_patch(const FrameQueueChange &change, const NativeFrameLayout &layout) {
    for (auto index = std::size_t{}; index != change.supplied.size(); ++index)
        if (change.supplied[index] && !is_key_field(layout, index))
            return true;
    return false;
}

void apply_frame_queue_payload(util::buffer_reader &reader, OutputState &output, const NativeFrameLayout &layout) {
    auto discard = util::scope_exit{[&]() noexcept {
        try {
            discard_frame_queue_payload(reader, layout);
        } catch (...) {
            std::terminate();
        }
    }};
    auto decoded = reader.fork();
    const auto count = decoded.read_raw<std::uint32_t>();
    auto &storage = *static_cast<NativeFrameStorage *>(output.mutable_data());
    const auto time = output.context().evaluation_time();
    auto &memory = output.allocator();
    if (count == 0) {
        if (!output.valid()) {
            storage.clear(time, memory);
            output.publish();
        }
        return;
    }

    for (auto position = std::uint32_t{}; position != count; ++position) {
        const auto change = read_frame_queue_change(decoded, layout);
        if (change.operation == FrameQueueOperation::remove) {
            const auto key = read_frame_literal_key(change.cells, layout);
            if (const auto id = storage.find_key(key); id && storage.erase_queue(*id, time, memory))
                output.publish();
            continue;
        }

        if (!layout.key_fields.empty()) {
            const auto key = read_frame_literal_key(change.cells, layout);
            if (const auto id = storage.find_key(key)) {
                if (has_non_key_patch(change, layout) && storage.set(*id, frame_patches(change, layout), time, memory))
                    output.publish();
                continue;
            }
        }

        const auto values = frame_cell_pointers(change);
        static_cast<void>(storage.add(values, time, memory));
        output.publish();
    }
}

void discard_column_queue_payload(util::buffer_reader &reader, const ScalarType &type) {
    const auto count = reader.read_raw<std::uint32_t>();
    for (auto position = std::uint32_t{}; position != count; ++position)
        tick_codec::discard_cell(reader, type);
}

template <cell_type T>
void project_column_queue_literal(const literal::TimeSeriesLiteral &literal, ScalarBufferProjectionContext &projection) {
    const auto values = column_queue_literal_values(literal);
    if (values.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error{"TSC queue literal has too many cells"};
    const auto count = static_cast<std::uint32_t>(values.size());
    project_raw(projection, count);
    for (const auto &value : values) {
        const auto cell = read_literal_cell<T>(value);
        tick_codec::project_cell<T>(cell, projection);
    }
}

template <cell_type T>
void write_column_queue_literal(const literal::TimeSeriesLiteral &literal, ScalarBufferWriteContext &writer,
                                allocator &memory) {
    const auto values = column_queue_literal_values(literal);
    if (values.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error{"TSC queue literal has too many cells"};
    const auto count = static_cast<std::uint32_t>(values.size());
    write_raw(writer, count);
    for (const auto &value : values) {
        const auto cell = read_literal_cell<T>(value);
        tick_codec::write_cell<T>(cell, writer, memory);
    }
}

template <cell_type T>
void apply_column_queue_payload(util::buffer_reader &reader, OutputState &output) {
    auto discard = util::scope_exit{[&]() noexcept {
        try {
            discard_column_queue_payload(reader, cpp_scalar_type<T>());
        } catch (...) {
            std::terminate();
        }
    }};
    auto decoded = reader.fork();
    const auto count = decoded.read_raw<std::uint32_t>();
    auto typed_output = Out<TSC<T>>{TimeSeriesOutputBinding{output}};
    if (count == 0) {
        if (!typed_output.valid()) {
            typed_output.clear();
        }
    } else {
        for (auto position = std::uint32_t{}; position != count; ++position) {
            const auto cell = tick_codec::read_cell<T>(decoded);
            static_cast<void>(typed_output.add(cell));
        }
    }
}

template <cell_type T>
bool column_queue_conflicts(const TimeSeriesRuntimeType::ConstItem &item, EvaluationTime now, bool incoming_valid,
                            util::buffer_reader &reader) {
    if (!incoming_valid)
        return item.last_updated.valid() && item.last_updated.time() == now;
    if (!item.last_updated.valid() && item.last_updated.time() == now)
        return true;
    const auto count = reader.read_raw<std::uint32_t>();
    for (auto position = std::uint32_t{}; position != count; ++position)
        static_cast<void>(tick_codec::read_cell<T>(reader));
    return false;
}

bool frame_queue_conflicts(const TimeSeriesRuntimeType::ConstItem &item, EvaluationTime now, bool incoming_valid,
                           util::buffer_reader &reader, const NativeFrameLayout &layout) {
    if (!incoming_valid)
        return item.last_updated.valid() && item.last_updated.time() == now;

    const auto *storage = static_cast<const NativeFrameStorage *>(item.value);
    const auto revalidates_this_cycle = !item.last_updated.valid() && item.last_updated.time() == now;
    const auto count = reader.read_raw<std::uint32_t>();
    if (count == 0)
        return revalidates_this_cycle && (storage == nullptr || !storage->valid());

    for (auto position = std::uint32_t{}; position != count; ++position) {
        const auto change = read_frame_queue_change(reader, layout);
        if (change.operation == FrameQueueOperation::remove) {
            if (storage == nullptr)
                continue;
            const auto key = read_frame_literal_key(change.cells, layout);
            if (const auto id = storage->find_key(key); id &&
                (revalidates_this_cycle ||
                 (storage->change_time() == now && storage->key_changed_this_cycle(key))))
                return true;
            continue;
        }

        auto effective = layout.key_fields.empty();
        auto key = std::vector<KeyValue>{};
        std::optional<row_id_t> id;
        if (!layout.key_fields.empty()) {
            key = read_frame_literal_key(change.cells, layout);
            id = storage == nullptr ? std::nullopt : storage->find_key(key);
            effective = !id || has_non_key_patch(change, layout);
        }
        if (!effective)
            continue;
        if (revalidates_this_cycle || (storage != nullptr && !layout.key_fields.empty() &&
                                       storage->change_time() == now && storage->key_changed_this_cycle(key)))
            return true;
    }
    return false;
}

template <cell_type T> void register_column_queue_runtime(const TimeSeriesRuntimeType &runtime) {
    auto &dispatch = TimeSeriesDispatch<TimeSeriesQueueOperations>::instance();
    auto discard = TimeSeriesQueueOperations::for_type([](util::buffer_reader &reader) {
        discard_column_queue_payload(reader, cpp_scalar_type<T>());
    });
    static_cast<void>(dispatch.add(runtime.type(), std::move(discard)));

    auto operations = TimeSeriesQueueOperations::for_runtime(
        &project_column_queue_literal<T>, &write_column_queue_literal<T>,
        [](const TimeSeriesRuntimeType::ConstItem &item, EvaluationTime now, bool incoming_valid,
           util::buffer_reader &reader) {
            return column_queue_conflicts<T>(item, now, incoming_valid, reader);
        },
        &apply_column_queue_payload<T>);
    static_cast<void>(dispatch.add(runtime, std::move(operations)));
}

} // namespace

template <typename T> void register_column_series_queue_runtime(const TimeSeriesRuntimeType &runtime) {
    register_column_queue_runtime<T>(runtime);
}

template BG_FRAME_EXPORT void register_column_series_queue_runtime<i64>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_queue_runtime<double>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_queue_runtime<bool>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_queue_runtime<string>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_queue_runtime<symbol>(const TimeSeriesRuntimeType &runtime);

void register_frame_series_queue_runtime(const TimeSeriesRuntimeType &runtime) {
    const auto &series = static_cast<const FrameSeriesType &>(runtime.type());
    const auto &layout = native_frame_layout(series.schema_type());
    auto &dispatch = TimeSeriesDispatch<TimeSeriesQueueOperations>::instance();
    auto discard = TimeSeriesQueueOperations::for_type([layout = std::addressof(layout)](util::buffer_reader &reader) {
        discard_frame_queue_payload(reader, *layout);
    });
    static_cast<void>(dispatch.add(runtime.type(), std::move(discard)));

    auto operations = TimeSeriesQueueOperations::for_runtime(
        [layout = std::addressof(layout)](const literal::TimeSeriesLiteral &literal,
                                          ScalarBufferProjectionContext &projection) {
            project_frame_queue_literal(literal, *layout, projection);
        },
        [layout = std::addressof(layout)](const literal::TimeSeriesLiteral &literal,
                                          ScalarBufferWriteContext &writer, allocator &memory) {
            write_frame_queue_literal(literal, *layout, writer, memory);
        },
        [layout = std::addressof(layout)](const TimeSeriesRuntimeType::ConstItem &item, EvaluationTime now,
                                          bool incoming_valid, util::buffer_reader &reader) {
            return frame_queue_conflicts(item, now, incoming_valid, reader, *layout);
        },
        [layout = std::addressof(layout)](util::buffer_reader &reader, OutputState &output) {
            apply_frame_queue_payload(reader, output, *layout);
        });
    static_cast<void>(dispatch.add(runtime, std::move(operations)));
}

} // namespace bg::frame::details
