#pragma once

#include <bg/frame/frame.hpp>
#include <bg/time_series/tick_serializer.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

namespace bg::frame::details::tick_codec {

using Cell = std::variant<std::monostate, std::optional<bool>, std::optional<i64>, std::optional<double>,
                          std::optional<std::string_view>, std::optional<symbol>>;

struct FrameChange {
    bg::details::TickOperation operation;
    row_id_t id{};
    std::vector<KeyValue> key;
    std::vector<Cell> cells;
};

template <cell_type T> std::optional<cell_value_t<T>> read_cell(util::buffer_reader &reader) {
    if (reader.read_raw<std::uint8_t>() == 0)
        return std::nullopt;
    if constexpr (std::same_as<T, string>) {
        const auto value = reader.peek<ScalarValue<string>>().view().value();
        reader.consume<ScalarValue<string>>();
        return value;
    } else {
        return reader.read_raw<cell_value_t<T>>();
    }
}

inline Cell read_frame_cell(util::buffer_reader &reader, const ScalarType &type) {
    if (std::addressof(type) == std::addressof(cpp_scalar_type<bool>()))
        return read_cell<bool>(reader);
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>()))
        return read_cell<i64>(reader);
    if (std::addressof(type) == std::addressof(cpp_scalar_type<double>()))
        return read_cell<double>(reader);
    if (std::addressof(type) == std::addressof(cpp_scalar_type<string>()))
        return read_cell<string>(reader);
    if (std::addressof(type) == std::addressof(cpp_scalar_type<symbol>()))
        return read_cell<symbol>(reader);
    throw std::invalid_argument{"frame tick payload has an unsupported cell type"};
}

inline void discard_cell(util::buffer_reader &reader, const ScalarType &type) {
    if (reader.read_raw<std::uint8_t>() == 0)
        return;
    if (std::addressof(type) == std::addressof(cpp_scalar_type<bool>())) {
        static_cast<void>(reader.read_raw<bool>());
        return;
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>())) {
        static_cast<void>(reader.read_raw<i64>());
        return;
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<double>())) {
        static_cast<void>(reader.read_raw<double>());
        return;
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<string>())) {
        reader.skip<ScalarValue<string>>();
        return;
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<symbol>())) {
        static_cast<void>(reader.read_raw<symbol>());
        return;
    }
    throw std::invalid_argument{"frame tick payload has an unsupported cell type"};
}

inline const bg::Schema &read_schema(util::buffer_reader &reader) {
    return *reader.read_raw<const bg::Schema *>();
}

inline std::vector<std::uint32_t> read_changed_columns(util::buffer_reader &reader) {
    const auto count = reader.read_raw<std::uint32_t>();
    auto columns = std::vector<std::uint32_t>{};
    columns.reserve(count);
    for (auto position = std::uint32_t{}; position != count; ++position) {
        columns.push_back(reader.read_raw<std::uint32_t>());
    }
    return columns;
}

inline void skip_changed_columns(util::buffer_reader &reader) {
    const auto count = reader.read_raw<std::uint32_t>();
    for (auto position = std::uint32_t{}; position != count; ++position)
        static_cast<void>(reader.read_raw<std::uint32_t>());
}

inline Cell cell_from_key(const KeyValue &key, const ScalarType &type) {
    if (std::holds_alternative<std::monostate>(key)) {
        if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>()))
            return std::optional<i64>{};
        if (std::addressof(type) == std::addressof(cpp_scalar_type<string>()))
            return std::optional<std::string_view>{};
        if (std::addressof(type) == std::addressof(cpp_scalar_type<symbol>()))
            return std::optional<symbol>{};
    }
    return std::visit(
        [](const auto &value) -> Cell {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, std::monostate>)
                return std::monostate{};
            else if constexpr (std::same_as<Value, i64>)
                return std::optional<i64>{value};
            else if constexpr (std::same_as<Value, symbol>)
                return std::optional<symbol>{value};
            else
                return std::optional<std::string_view>{value.view().value()};
        },
        key);
}

inline void read_key(util::buffer_reader &reader, const NativeFrameLayout &layout, std::vector<KeyValue> &key) {
    key.reserve(layout.key_fields.size());
    for (const auto index : layout.key_fields) {
        const auto &type = *layout.fields[index].cell_type;
        if (std::addressof(type) == std::addressof(cpp_scalar_type<string>())) {
            if (reader.read_raw<std::uint8_t>() == 0) {
                key.emplace_back(std::monostate{});
                continue;
            }
            const auto &value = reader.peek<ScalarValue<string>>();
            key.emplace_back(ScalarValue<string>{value.view()});
            reader.consume<ScalarValue<string>>();
            continue;
        }
        if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>())) {
            const auto value = read_cell<i64>(reader);
            key.emplace_back(value ? KeyValue{*value} : KeyValue{std::monostate{}});
            continue;
        }
        if (std::addressof(type) == std::addressof(cpp_scalar_type<symbol>())) {
            const auto value = read_cell<symbol>(reader);
            key.emplace_back(value ? KeyValue{*value} : KeyValue{std::monostate{}});
            continue;
        }
        throw std::invalid_argument{"frame key has an unsupported cell type"};
    }
}

inline FrameChange read_change(util::buffer_reader &reader, const bg::Schema &schema,
                               const NativeFrameLayout &layout, bg::details::TickHeader header) {
    auto change = FrameChange{header.operation, reader.read_raw<row_id_t>(), {},
                              std::vector<Cell>(schema.field_count())};
    if (header.operation != bg::details::TickOperation::remove)
        read_key(reader, layout, change.key);

    if (header.operation == bg::details::TickOperation::add) {
        for (auto key_position = std::size_t{}; key_position != layout.key_fields.size(); ++key_position) {
            const auto index = layout.key_fields[key_position];
            change.cells[index] = cell_from_key(change.key[key_position], *layout.fields[index].cell_type);
        }
        for (auto index = std::size_t{}; index != schema.field_count(); ++index) {
            if (std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                change.cells[index] = read_frame_cell(reader, *layout.fields[index].cell_type);
        }
        return change;
    }

    if (header.operation != bg::details::TickOperation::tick)
        return change;

    const auto field_count = reader.read_raw<std::uint32_t>();
    for (auto count = std::uint32_t{}; count != field_count; ++count) {
        const auto index = reader.read_raw<std::uint32_t>();
        change.cells[index] = read_frame_cell(reader, *layout.fields[index].cell_type);
    }
    return change;
}

} // namespace bg::frame::details::tick_codec
