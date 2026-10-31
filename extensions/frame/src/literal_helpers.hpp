#pragma once

#include <bg/frame/frame.hpp>
#include <bg/frame/literal.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/time_series/details/scalar_literal.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

namespace bg::frame::details {

using FrameLiteralCell = std::variant<std::optional<bool>, std::optional<i64>, std::optional<double>,
                                      std::optional<std::string_view>, std::optional<symbol>>;

inline void initialize_frame_literal_cells(const NativeFrameLayout &layout, std::vector<FrameLiteralCell> &cells) {
    cells.resize(layout.fields.size());
    for (auto index = std::size_t{}; index != layout.fields.size(); ++index) {
        visit_cell_type(*layout.fields[index].cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
            cells[index].emplace<std::optional<cell_value_t<Cell>>>();
        });
    }
}

inline const void *frame_literal_cell_pointer(const FrameLiteralCell &cell) {
    return std::visit([](const auto &value) -> const void * { return std::addressof(value); }, cell);
}

inline KeyValue frame_literal_key_value(const FrameLiteralCell &cell, const ScalarType &type) {
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>())) {
        const auto &value = std::get<std::optional<i64>>(cell);
        return value ? KeyValue{*value} : KeyValue{std::monostate{}};
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<string>())) {
        const auto &value = std::get<std::optional<std::string_view>>(cell);
        return value ? KeyValue{ScalarValue<string>{*value}} : KeyValue{std::monostate{}};
    }
    if (std::addressof(type) == std::addressof(cpp_scalar_type<symbol>())) {
        const auto &value = std::get<std::optional<symbol>>(cell);
        return value ? KeyValue{*value} : KeyValue{std::monostate{}};
    }
    throw std::invalid_argument{"frame literal key has an unsupported cell type"};
}

inline std::vector<KeyValue> read_frame_literal_key(std::span<const FrameLiteralCell> cells,
                                                    const NativeFrameLayout &layout) {
    auto key = std::vector<KeyValue>{};
    key.reserve(layout.key_fields.size());
    for (const auto index : layout.key_fields)
        key.push_back(frame_literal_key_value(cells[index], *layout.fields[index].cell_type));
    return key;
}

inline std::string_view read_literal_field_name(const literal::ScalarLiteral &value) {
    return value.visit(
        [](const literal::ScalarLiteralItem &item) {
            return item.visit_scalar([](auto *source) -> std::string_view {
                const auto &data = bg::details::scalar_data(*source);
                if (std::addressof(data.type()) == std::addressof(cpp_scalar_type<string>()))
                    return bg::details::scalar_data_value<string>(data);
                if (std::addressof(data.type()) == std::addressof(cpp_scalar_type<symbol>()))
                    return bg::details::scalar_data_value<symbol>(data).text();
                throw std::invalid_argument{"frame field name must be a string or symbol"};
            }).value();
        },
        [](std::span<const literal::ScalarLiteral>) -> std::string_view {
            throw std::invalid_argument{"frame field name must be a string or symbol"};
        }).value();
}

template <cell_type T> std::optional<cell_value_t<T>> read_literal_cell(const literal::ScalarLiteral &value) {
    auto result = std::optional<cell_value_t<T>>{};
    value.visit(
        [&](const literal::ScalarLiteralItem &item) {
            item.visit_scalar([&](auto *source) {
                const auto &data = bg::details::scalar_data(*source);
                if (const auto *sentinel = literal::Sentinel::from(data);
                    sentinel != nullptr && *sentinel == NULL_VALUE)
                    return;
                if (std::addressof(data.type()) != std::addressof(cpp_scalar_type<T>()))
                    throw std::invalid_argument{"frame cell type does not match the requested cell type"};
                result.emplace(bg::details::scalar_data_value<T>(data));
            });
        },
        [](std::span<const literal::ScalarLiteral>) {
            throw std::invalid_argument{"frame cells must be scalar literals"};
        });
    return result;
}

inline void read_frame_literal_cell(const literal::ScalarLiteral &literal, const ScalarType &type,
                                   FrameLiteralCell &destination) {
    visit_cell_type(type, [&]<typename Cell>(std::type_identity<Cell>) {
        destination.emplace<std::optional<cell_value_t<Cell>>>(read_literal_cell<Cell>(literal));
    });
}

inline std::vector<const literal::ScalarLiteral *>
read_named_frame_fields(const literal::ScalarLiteral &value, const bg::Schema &schema,
                        const NativeFrameLayout &layout, bool key_fields_only) {
    const auto entries = value.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
            throw std::invalid_argument{"frame row literal must contain named fields"};
        },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();

    auto fields = std::vector<const literal::ScalarLiteral *>(schema.field_count(), nullptr);
    for (const auto &entry : entries) {
        const auto pair = bg::details::scalar_literal::pair(
            entry, "frame row literal entries must contain a field name and value");
        const auto field_name = read_literal_field_name(*pair.first);
        const auto *field = schema.find(symbol{field_name});
        if (field == nullptr)
            throw std::invalid_argument{"frame literal contains an unknown column"};
        const auto index = static_cast<std::size_t>(field - schema.fields().data());
        if (key_fields_only && std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
            throw std::invalid_argument{"frame removal literal may contain only key columns"};
        if (fields[index] != nullptr)
            throw std::invalid_argument{"frame literal contains a duplicate column"};
        fields[index] = pair.second;
    }

    for (const auto index : layout.key_fields)
        if (fields[index] == nullptr)
            throw std::invalid_argument{"frame literal omits a required key column"};
    return fields;
}

inline std::vector<KeyValue> read_literal_key(std::span<const literal::ScalarLiteral *const> fields,
                                              const NativeFrameLayout &layout) {
    auto key = std::vector<KeyValue>{};
    key.reserve(layout.key_fields.size());
    for (const auto index : layout.key_fields) {
        visit_cell_type(*layout.fields[index].cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
            const auto value = read_literal_cell<Cell>(*fields[index]);
            if constexpr (std::same_as<Cell, string>)
                key.push_back(value ? KeyValue{ScalarValue<string>{*value}} : KeyValue{std::monostate{}});
            else if constexpr (std::same_as<Cell, i64> || std::same_as<Cell, symbol>)
                key.push_back(value ? KeyValue{*value} : KeyValue{std::monostate{}});
            else
                throw std::invalid_argument{"frame key has an unsupported cell type"};
        });
    }
    return key;
}

} // namespace bg::frame::details
