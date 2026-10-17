#include <bg/frame/literal.hpp>

#include <bg/frame/column.hpp>
#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/details/value_access.hpp>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace bg::frame {

const literal::Sentinel NULL_VALUE{symbol{"frame.null"}};

namespace {

auto literal_registration_mutex = std::mutex{};

template <cell_type T> std::optional<T> read_cell(const literal::ScalarLiteral &value) {
    auto result = std::optional<T>{};
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
                if (auto cell = read_cell<T>(cells[position])) {
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

std::string_view read_field_name(const literal::ScalarLiteral &literal) {
    return literal.visit(
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

                const auto field_name = read_field_name(*name);
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

} // namespace

namespace details {

void register_column_literal_type(const ScalarType &type) {
    const auto lock = std::lock_guard{literal_registration_mutex};
    static_cast<void>(ScalarDispatch<ScalarLiteralOperations>::instance().add(
        type, ScalarLiteralOperations{&construct_column_literal}));
}

void register_frame_literal_type(const ScalarType &type) {
    const auto lock = std::lock_guard{literal_registration_mutex};
    static_cast<void>(ScalarDispatch<ScalarLiteralOperations>::instance().add(
        type, ScalarLiteralOperations{&construct_frame_literal}));
}

void register_literal_runtime(const ScalarRuntimeType &runtime, ScalarLiteralOperations operations) {
    const auto lock = std::lock_guard{literal_registration_mutex};
    static_cast<void>(ScalarDispatch<ScalarLiteralOperations>::instance().add(runtime, operations));
}

} // namespace details
} // namespace bg::frame
