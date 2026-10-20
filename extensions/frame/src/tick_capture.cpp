#include <bg/frame/frame.hpp>
#include <bg/frame/literal.hpp>

#include <bg/testing/time_series_tick_comparison.hpp>
#include <bg/time_series/tick_operations.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/util/static_storage.hpp>
#include "literal_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace bg::frame::details {
namespace {

using Cell = std::variant<std::monostate, std::optional<i64>, std::optional<double>>;

struct FrameChange {
    bg::details::TickOperation operation;
    std::vector<KeyValue> key;
    std::vector<Cell> cells;
};

struct ExpectedFrameChange {
    bool removal{};
    std::vector<KeyValue> key;
    std::vector<Cell> cells;
};

std::mutex &capture_registration_mutex() {
    static bg::util::static_storage<std::mutex> mutex;
    return mutex.get();
}

template <typename T> void write_cell(ScalarBufferWriteContext &writer, std::optional<T> value) {
    const auto valid = static_cast<std::uint8_t>(value.has_value());
    bg::details::write_bytes(writer, valid);
    if (value)
        bg::details::write_bytes(writer, *value);
}

void write_column_cell(ScalarBufferWriteContext &writer, const NativeFrameStorage &storage,
                       std::size_t index, row_id_t id) {
    const auto &field = storage.layout().fields[index];
    const auto *native_field = storage.native_field(index);
    const auto &rows = storage.rows();
    const auto &cell_type = *field.cell_type;
    if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>())) {
        write_cell(writer, static_cast<const NativeColumnData<i64> *>(native_field)->cell(rows, id));
        return;
    }
    write_cell(writer, static_cast<const NativeColumnData<double> *>(native_field)->cell(rows, id));
}

void write_live_key(ScalarBufferWriteContext &writer, const NativeFrameStorage &storage,
                    const NativeFrameLayout &layout, row_id_t id) {
    for (const auto index : layout.key_fields)
        write_column_cell(writer, storage, index, id);
}

void write_retained_key(ScalarBufferWriteContext &writer, std::span<const KeyValue> key) {
    for (const auto &cell : key)
        write_cell(writer, std::get_if<i64>(&cell) == nullptr
                              ? std::optional<i64>{}
                              : std::optional<i64>{std::get<i64>(cell)});
}

void write_frame_tick(const In<TSF<SCHEMA>> &input, ScalarBufferWriteContext &writer) {
    const auto &storage = *static_cast<const NativeFrameStorage *>(input.input_data().data());
    const auto &layout = storage.layout();

    bg::details::write_bytes(writer, layout.schema);
    if (storage.change_time() != input.last_updated()) {
        bg::details::TickHeader{bg::details::TickOperation::end}.write(writer);
        return;
    }

    for (const auto id : storage.added()) {
        bg::details::TickHeader{bg::details::TickOperation::add}.write(writer);
        write_live_key(writer, storage, layout, id);
        for (auto index = std::size_t{}; index != layout.fields.size(); ++index) {
            if (std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                write_column_cell(writer, storage, index, id);
        }
    }

    for (const auto id : storage.changed()) {
        if (storage.added().contains(id))
            continue;
        bg::details::TickHeader{bg::details::TickOperation::tick}.write(writer);
        write_live_key(writer, storage, layout, id);

        auto modified_count = std::uint32_t{};
        for (const auto index : storage.changed_columns()) {
            if (storage.modified(index)->contains(id))
                ++modified_count;
        }
        bg::details::write_bytes(writer, modified_count);
        for (const auto index : storage.changed_columns()) {
            const auto *modified = storage.modified(index);
            if (!modified->contains(id))
                continue;
            bg::details::write_bytes(writer, static_cast<std::uint32_t>(index));
            write_column_cell(writer, storage, index, id);
        }
    }

    for (const auto id : storage.removed()) {
        bg::details::TickHeader{bg::details::TickOperation::remove}.write(writer);
        write_retained_key(writer, storage.removed_key(id));
    }
    bg::details::TickHeader{bg::details::TickOperation::end}.write(writer);
}

template <typename T> std::optional<T> read_cell(util::buffer_reader &reader) {
    if (reader.read_raw<std::uint8_t>() == 0)
        return std::nullopt;
    return reader.read_raw<T>();
}

Cell read_cell(util::buffer_reader &reader, const ScalarType &type) {
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>()))
        return read_cell<i64>(reader);
    if (std::addressof(type) == std::addressof(cpp_scalar_type<double>()))
        return read_cell<double>(reader);
    throw std::invalid_argument{"frame tick payload has an unsupported cell type"};
}

void discard_cell(util::buffer_reader &reader, const ScalarType &type) {
    if (reader.read_raw<std::uint8_t>() == 0)
        return;
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>())) {
        static_cast<void>(reader.read_raw<i64>());
        return;
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<double>())) {
        static_cast<void>(reader.read_raw<double>());
        return;
    }
    throw std::invalid_argument{"frame tick payload has an unsupported cell type"};
}

const bg::Schema &read_schema(util::buffer_reader &reader) {
    return *reader.read_raw<const bg::Schema *>();
}

void read_key(util::buffer_reader &reader, const NativeFrameLayout &layout, std::vector<KeyValue> &key) {
    key.reserve(layout.key_fields.size());
    for (auto index : layout.key_fields) {
        const auto value = read_cell<i64>(reader);
        key.push_back(value ? KeyValue{*value} : KeyValue{std::monostate{}});
    }
}

FrameChange read_change(util::buffer_reader &reader, const bg::Schema &schema,
                        const NativeFrameLayout &layout, bg::details::TickHeader header) {
    auto change = FrameChange{header.operation, {}, std::vector<Cell>(schema.field_count())};
    read_key(reader, layout, change.key);

    if (header.operation == bg::details::TickOperation::add) {
        for (auto index = std::size_t{}; index != schema.field_count(); ++index) {
            if (std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                change.cells[index] = read_cell(reader, *layout.fields[index].cell_type);
        }
        return change;
    }

    if (header.operation != bg::details::TickOperation::tick)
        return change;

    const auto field_count = reader.read_raw<std::uint32_t>();
    for (auto count = std::uint32_t{}; count != field_count; ++count) {
        const auto index = reader.read_raw<std::uint32_t>();
        change.cells[index] = read_cell(reader, *layout.fields[index].cell_type);
    }
    return change;
}

void discard_frame_tick(util::buffer_reader &reader) {
    const auto &schema = read_schema(reader);
    const auto &layout = native_frame_layout(schema);
    for (;;) {
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation == bg::details::TickOperation::end)
            return;

        for (auto index : layout.key_fields)
            discard_cell(reader, *layout.fields[index].cell_type);
        if (header.operation == bg::details::TickOperation::add) {
            for (auto index = std::size_t{}; index != schema.field_count(); ++index)
                if (std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                    discard_cell(reader, *layout.fields[index].cell_type);
            continue;
        }
        if (header.operation == bg::details::TickOperation::remove)
            continue;
        if (header.operation != bg::details::TickOperation::tick)
            throw std::invalid_argument{"invalid frame tick operation"};

        const auto field_count = reader.read_raw<std::uint32_t>();
        for (auto count = std::uint32_t{}; count != field_count; ++count) {
            const auto index = reader.read_raw<std::uint32_t>();
            discard_cell(reader, *layout.fields[index].cell_type);
        }
    }
}

std::vector<ExpectedFrameChange> read_expected_changes(const literal::TimeSeriesLiteral &expected,
                                                       const bg::Schema &schema,
                                                       const NativeFrameLayout &layout) {
    const auto entries = expected.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
            throw std::invalid_argument{"TSF delta literal must contain a sequence of row changes"};
        },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();

    auto changes = std::vector<ExpectedFrameChange>{};
    changes.reserve(entries.size());
    for (const auto &entry : entries) {
        const auto tagged = literal::TaggedLiteral::from(entry);
        if (tagged && tagged->sentinel != literal::REMOVE)
            throw std::invalid_argument{"TSF delta literal does not accept this tagged literal"};
        const auto removal = tagged.has_value();
        const auto &row = removal ? tagged->argument : entry;
        const auto fields = read_named_frame_fields(row, schema, layout, removal);
        auto change = ExpectedFrameChange{.removal = removal,
                                          .key = read_literal_key(fields, layout),
                                          .cells = std::vector<Cell>(schema.field_count())};
        if (!removal) {
            for (auto index = std::size_t{}; index != fields.size(); ++index) {
                if (fields[index] == nullptr || std::ranges::find(layout.key_fields, index) != layout.key_fields.end())
                    continue;
                if (std::addressof(*layout.fields[index].cell_type) == std::addressof(cpp_scalar_type<i64>()))
                    change.cells[index] = read_literal_cell<i64>(*fields[index]);
                else
                    change.cells[index] = read_literal_cell<double>(*fields[index]);
            }
        }
        changes.push_back(std::move(change));
    }

    for (auto index = std::size_t{}; index != changes.size(); ++index) {
        for (auto candidate = index + 1; candidate != changes.size(); ++candidate) {
            if (changes[index].removal == changes[candidate].removal &&
                changes[index].key == changes[candidate].key)
                throw std::invalid_argument{"TSF delta literal contains duplicate row keys"};
        }
    }
    return changes;
}

bool matches_expected(const FrameChange &actual, const ExpectedFrameChange &expected,
                      const NativeFrameLayout &layout) {
    const auto is_removal = actual.operation == bg::details::TickOperation::remove;
    if (is_removal != expected.removal || actual.key != expected.key)
        return false;
    if (is_removal)
        return true;

    const auto actual_addition = actual.operation == bg::details::TickOperation::add;
    for (auto index = std::size_t{}; index != expected.cells.size(); ++index) {
        if (std::ranges::find(layout.key_fields, index) != layout.key_fields.end())
            continue;

        if (actual_addition && std::holds_alternative<std::monostate>(expected.cells[index])) {
            if (std::addressof(*layout.fields[index].cell_type) == std::addressof(cpp_scalar_type<i64>())) {
                if (std::get<std::optional<i64>>(actual.cells[index]).has_value())
                    return false;
            } else if (std::get<std::optional<double>>(actual.cells[index]).has_value()) {
                return false;
            }
            continue;
        }
        if (actual.cells[index] != expected.cells[index])
            return false;
    }
    return true;
}

bool compare_frame_tick(util::buffer_reader &reader, const TimeSeriesType &,
                        const literal::TimeSeriesLiteral &expected) {
    const auto &schema = read_schema(reader);
    const auto &layout = native_frame_layout(schema);
    auto expected_changes = read_expected_changes(expected, schema, layout);
    auto matched = std::vector<bool>(expected_changes.size());

    for (;;) {
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation == bg::details::TickOperation::end)
            return std::ranges::all_of(matched, [](bool value) { return value; });
        if (header.operation != bg::details::TickOperation::add &&
            header.operation != bg::details::TickOperation::tick &&
            header.operation != bg::details::TickOperation::remove)
            return false;

        auto actual = read_change(reader, schema, layout, header);
        auto candidate = expected_changes.size();
        for (auto index = std::size_t{}; index != expected_changes.size(); ++index) {
            if (matched[index] || !matches_expected(actual, expected_changes[index], layout))
                continue;
            candidate = index;
            break;
        }
        if (candidate == expected_changes.size())
            return false;
        matched[candidate] = true;
    }
}

} // namespace

void register_frame_series_tick_comparison(const TimeSeriesType &type) {
    const auto lock = std::lock_guard{capture_registration_mutex()};
    static_cast<void>(TimeSeriesDispatch<TimeSeriesTickOperations>::instance().add(
        type, TimeSeriesTickOperations::for_discard(&discard_frame_tick)));
    static_cast<void>(TimeSeriesDispatch<testing::TickLiteralComparison>::instance().add(
        type, testing::TickLiteralComparison{&compare_frame_tick}));
}

void register_frame_series_tick_writer(const TimeSeriesRuntimeType &runtime) {
    const auto &series_type = static_cast<const FrameSeriesType &>(runtime.type());
    if (native_frame_layout(series_type.schema_type()).key_fields.empty())
        return;

    auto operations = TimeSeriesTickOperations::for_write<TSF<SCHEMA>>(
        [](const In<TSF<SCHEMA>> &input, ScalarBufferWriteContext &writer, allocator &) {
            write_frame_tick(input, writer);
        });
    const auto lock = std::lock_guard{capture_registration_mutex()};
    static_cast<void>(TimeSeriesDispatch<TimeSeriesTickOperations>::instance().add(runtime, std::move(operations)));
}

} // namespace bg::frame::details
