#include <bg/frame/frame.hpp>
#include <bg/frame/literal.hpp>
#include <bg/frame/tsc.hpp>

#include <bg/testing/time_series_tick_comparison.hpp>
#include <bg/time_series/tick_operations.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include "literal_helpers.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace bg::frame::details {
namespace {

using Cell = std::variant<std::monostate, std::optional<bool>, std::optional<i64>, std::optional<double>,
                          std::optional<std::string_view>, std::optional<symbol>>;

struct FrameChange {
    bg::details::TickOperation operation;
    row_id_t id{};
    std::vector<KeyValue> key;
    std::vector<Cell> cells;
};

struct ExpectedFrameChange {
    bool removal{};
    std::vector<KeyValue> key;
    std::vector<Cell> cells;
};

enum class RowEventKind : std::uint8_t { add, modify, remove, invalidate };

struct RowEvent {
    std::size_t tick{};
    RowEventKind kind{};
    std::vector<Cell> cells;

    bool operator==(const RowEvent &) const = default;
};

struct RowHistory {
    std::vector<RowEvent> events;
};

class RowHistoryMatcher {
  public:
    bool expected_seen(std::string_view name) const {
        return expected_by_name_.contains(name);
    }

    bool expected_active(std::string_view name) const {
        const auto found = expected_by_name_.find(name);
        return found != expected_by_name_.end() && expected_[found->second].active;
    }

    void expect_add(std::string name, std::vector<Cell> cells, std::size_t tick) {
        if (expected_seen(name))
            throw std::invalid_argument{"named row expectation reused a name"};
        expected_.push_back({std::move(name), {}, true});
        const auto index = expected_.size() - 1;
        expected_by_name_.emplace(expected_.back().name, index);
        append(expected_[index].history, RowEventKind::add, std::move(cells), tick);
        ++expected_counts_[0];
    }

    void expect_modify(std::string_view name, std::vector<Cell> cells, std::size_t tick) {
        auto &history = active_expected(name);
        append(history.history, RowEventKind::modify, std::move(cells), tick);
        ++expected_counts_[1];
    }

    void expect_remove(std::string_view name, std::size_t tick) {
        auto &history = active_expected(name);
        append(history.history, RowEventKind::remove, {}, tick);
        history.active = false;
        ++expected_counts_[2];
    }

    void add_actual(row_id_t id, std::vector<Cell> cells, std::size_t tick) {
        if (actual_by_id_.contains(id))
            throw std::invalid_argument{"frame tick added an active row ID"};
        actual_.push_back({});
        const auto index = actual_.size() - 1;
        actual_by_id_.emplace(id, index);
        append(actual_.back(), RowEventKind::add, std::move(cells), tick);
        ++actual_counts_[0];
    }

    void modify_actual(row_id_t id, std::vector<Cell> cells, std::size_t tick) {
        append(active_actual_history(id), RowEventKind::modify, std::move(cells), tick);
        ++actual_counts_[1];
    }

    void remove_actual(row_id_t id, std::size_t tick) {
        append(active_actual_history(id), RowEventKind::remove, {}, tick);
        actual_by_id_.erase(id);
        ++actual_counts_[2];
    }

    bool finish_tick() {
        const auto matches = expected_counts_ == actual_counts_;
        expected_counts_ = {};
        actual_counts_ = {};
        return matches;
    }

    void invalidate(std::size_t tick) {
        for (auto &history : expected_) {
            if (!history.active)
                continue;
            append(history.history, RowEventKind::invalidate, {}, tick);
            history.active = false;
        }
        for (const auto &[id, index] : actual_by_id_) {
            static_cast<void>(id);
            append(actual_[index], RowEventKind::invalidate, {}, tick);
        }
        actual_by_id_.clear();
    }

    bool finish() const {
        if (expected_.size() != actual_.size())
            return false;

        auto matched = std::vector<bool>(actual_.size());
        for (const auto &expected : expected_) {
            auto found = false;
            for (auto index = std::size_t{}; index != actual_.size(); ++index) {
                if (matched[index] || expected.history.events != actual_[index].events)
                    continue;
                matched[index] = true;
                found = true;
                break;
            }
            if (!found)
                return false;
        }
        return true;
    }

  private:
    struct ExpectedHistory {
        std::string name;
        RowHistory history;
        bool active{};
    };

    static void append(RowHistory &history, RowEventKind kind, std::vector<Cell> cells, std::size_t tick) {
        history.events.push_back({tick, kind, std::move(cells)});
    }

    ExpectedHistory &active_expected(std::string_view name) {
        const auto found = expected_by_name_.find(name);
        if (found == expected_by_name_.end() || !expected_[found->second].active)
            throw std::invalid_argument{"named row expectation does not name an active row"};
        return expected_[found->second];
    }

    RowHistory &active_actual_history(row_id_t id) {
        const auto found = actual_by_id_.find(id);
        if (found == actual_by_id_.end())
            throw std::invalid_argument{"frame tick changed or removed an inactive row ID"};
        return actual_[found->second];
    }

    struct NameHash {
        using is_transparent = void;

        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::vector<ExpectedHistory> expected_;
    std::vector<RowHistory> actual_;
    std::unordered_map<std::string, std::size_t, NameHash, std::equal_to<>> expected_by_name_;
    std::unordered_map<row_id_t, std::size_t> actual_by_id_;
    std::array<std::size_t, 3> expected_counts_{};
    std::array<std::size_t, 3> actual_counts_{};
};

using FrameColumnView = std::variant<ColumnSeriesView<bool>, ColumnSeriesView<i64>, ColumnSeriesView<double>,
                                     ColumnSeriesView<string>, ColumnSeriesView<symbol>>;

std::vector<FrameColumnView> bind_frame_columns(const FrameReadStorage &storage) {
    auto columns = std::vector<FrameColumnView>{};
    columns.reserve(storage.schema->field_count());
    for (auto index = std::size_t{}; index != storage.schema->field_count(); ++index) {
        const auto binding = storage.column(index);
        columns.push_back(visit_cell_type(*binding.cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
            auto view = std::optional<ColumnSeriesView<Cell>>{};
            binding.bind_column_series_view(binding.payload, std::addressof(view));
            return FrameColumnView{*view};
        }));
    }
    return columns;
}

template <typename T>
void write_column_cell(ScalarBufferWriteContext &writer, const ColumnSeriesView<T> &column, row_id_t id,
                       allocator &memory) {
    const auto value = column.cell(id);
    const auto valid = static_cast<std::uint8_t>(value.has_value());
    bg::details::write_bytes(writer, valid);
    if (!value)
        return;

    if constexpr (std::same_as<T, string>) {
        const auto &storage = column.scalar_storage();
        const auto &stored = storage.native->stored(*storage.physical_rows, id);
        writer.write(ScalarValue<string>{Scalar<string>::from(stored), memory});
    } else {
        bg::details::write_bytes(writer, *value);
    }
}

void write_column_cell(ScalarBufferWriteContext &writer, const std::vector<FrameColumnView> &columns,
                       std::size_t index, row_id_t id, allocator &memory) {
    std::visit(
        [&](const auto &column) {
            write_column_cell(writer, column, id, memory);
        },
        columns[index]);
}

void write_live_key(ScalarBufferWriteContext &writer, const std::vector<FrameColumnView> &columns,
                    const NativeFrameLayout &layout, row_id_t id, allocator &memory) {
    for (const auto index : layout.key_fields)
        write_column_cell(writer, columns, index, id, memory);
}

void write_frame_tick(const In<TSF<SCHEMA>> &input, ScalarBufferWriteContext &writer, allocator &memory) {
    const auto storage = input.extension_descriptor();
    const auto &layout = native_frame_layout(*storage.schema);

    bg::details::write_bytes(writer, storage.schema);
    if (storage.change_time != input.last_updated()) {
        bg::details::TickHeader{bg::details::TickOperation::end}.write(writer);
        return;
    }
    const auto columns = bind_frame_columns(storage);

    if (storage.removed != nullptr)
        for (const auto id : *storage.removed) {
            bg::details::TickHeader{bg::details::TickOperation::remove}.write(writer);
            bg::details::write_bytes(writer, id);
        }

    if (storage.added != nullptr)
        for (const auto id : *storage.added) {
            bg::details::TickHeader{bg::details::TickOperation::add}.write(writer);
            bg::details::write_bytes(writer, id);
            write_live_key(writer, columns, layout, id, memory);
            for (auto index = std::size_t{}; index != layout.fields.size(); ++index) {
                if (std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                    write_column_cell(writer, columns, index, id, memory);
            }
        }

    auto changed_columns = std::vector<std::size_t>{};
    storage.each_changed_column(std::addressof(changed_columns), [](void *context, std::size_t index) {
        static_cast<std::vector<std::size_t> *>(context)->push_back(index);
    });
    if (storage.changed != nullptr) {
        for (const auto id : *storage.changed) {
            if ((storage.added != nullptr && storage.added->contains(id)) ||
                (storage.removed != nullptr && storage.removed->contains(id)))
                continue;
            bg::details::TickHeader{bg::details::TickOperation::tick}.write(writer);
            bg::details::write_bytes(writer, id);
            write_live_key(writer, columns, layout, id, memory);

            auto modified_count = std::uint32_t{};
            for (const auto index : changed_columns) {
                const auto *modified = storage.modified_at(index);
                if (modified != nullptr && modified->contains(id))
                    ++modified_count;
            }
            bg::details::write_bytes(writer, modified_count);
            for (const auto index : changed_columns) {
                const auto *modified = storage.modified_at(index);
                if (modified == nullptr || !modified->contains(id))
                    continue;
                bg::details::write_bytes(writer, static_cast<std::uint32_t>(index));
                write_column_cell(writer, columns, index, id, memory);
            }
        }
    }
    bg::details::TickHeader{bg::details::TickOperation::end}.write(writer);
}

template <cell_type T>
void write_column_tick(const In<TSC<T>> &input, ScalarBufferWriteContext &writer, allocator &memory) {
    const auto descriptor = input.extension_descriptor();
    const auto storage = ColumnSeriesView<T>{descriptor.storage};
    if (storage.change_time() != input.last_updated()) {
        bg::details::TickHeader{bg::details::TickOperation::end}.write(writer);
        return;
    }

    for (const auto id : storage.removed()) {
        bg::details::TickHeader{bg::details::TickOperation::remove}.write(writer);
        bg::details::write_bytes(writer, id);
    }
    for (const auto id : storage.added()) {
        bg::details::TickHeader{bg::details::TickOperation::add}.write(writer);
        bg::details::write_bytes(writer, id);
        write_column_cell(writer, storage, id, memory);
    }
    for (const auto id : storage.modified()) {
        if (storage.added().contains(id))
            continue;
        bg::details::TickHeader{bg::details::TickOperation::tick}.write(writer);
        bg::details::write_bytes(writer, id);
        write_column_cell(writer, storage, id, memory);
    }
    bg::details::TickHeader{bg::details::TickOperation::end}.write(writer);
}

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

Cell read_cell(util::buffer_reader &reader, const ScalarType &type) {
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

void discard_cell(util::buffer_reader &reader, const ScalarType &type) {
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

const bg::Schema &read_schema(util::buffer_reader &reader) {
    return *reader.read_raw<const bg::Schema *>();
}

void read_key(util::buffer_reader &reader, const NativeFrameLayout &layout, std::vector<KeyValue> &key) {
    key.reserve(layout.key_fields.size());
    for (auto index : layout.key_fields) {
        visit_cell_type(*layout.fields[index].cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
            if constexpr (std::same_as<Cell, string>) {
                if (reader.read_raw<std::uint8_t>() == 0) {
                    key.emplace_back(std::monostate{});
                    return;
                }
                const auto &value = reader.peek<ScalarValue<string>>();
                key.emplace_back(ScalarValue<string>{value.view()});
                reader.consume<ScalarValue<string>>();
            } else if constexpr (std::same_as<Cell, i64> || std::same_as<Cell, symbol>) {
                const auto value = read_cell<Cell>(reader);
                key.push_back(value ? KeyValue{*value} : KeyValue{std::monostate{}});
            } else {
                throw std::invalid_argument{"frame key has an unsupported cell type"};
            }
        });
    }
}

FrameChange read_change(util::buffer_reader &reader, const bg::Schema &schema,
                        const NativeFrameLayout &layout, bg::details::TickHeader header) {
    auto change = FrameChange{header.operation, reader.read_raw<row_id_t>(), {},
                              std::vector<Cell>(schema.field_count())};
    if (header.operation != bg::details::TickOperation::remove)
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

        static_cast<void>(reader.read_raw<row_id_t>());
        if (header.operation == bg::details::TickOperation::remove)
            continue;
        for (auto index : layout.key_fields)
            discard_cell(reader, *layout.fields[index].cell_type);
        if (header.operation == bg::details::TickOperation::add) {
            for (auto index = std::size_t{}; index != schema.field_count(); ++index)
                if (std::ranges::find(layout.key_fields, index) == layout.key_fields.end())
                    discard_cell(reader, *layout.fields[index].cell_type);
            continue;
        }
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
                visit_cell_type(*layout.fields[index].cell_type, [&]<typename Cell>(std::type_identity<Cell>) {
                    change.cells[index] = read_literal_cell<Cell>(*fields[index]);
                });
            }
        }
        changes.push_back(std::move(change));
    }

    for (auto index = std::size_t{}; index != changes.size(); ++index) {
        for (auto candidate = index + 1; candidate != changes.size(); ++candidate) {
            if (changes[index].removal == changes[candidate].removal &&
                key_values_equal(changes[index].key, changes[candidate].key))
                throw std::invalid_argument{"TSF delta literal contains duplicate row keys"};
        }
    }
    return changes;
}

bool matches_expected(const FrameChange &actual, const ExpectedFrameChange &expected,
                      const NativeFrameLayout &layout) {
    const auto is_removal = actual.operation == bg::details::TickOperation::remove;
    if (is_removal != expected.removal || !key_values_equal(actual.key, expected.key))
        return false;
    if (is_removal)
        return true;

    const auto actual_addition = actual.operation == bg::details::TickOperation::add;
    for (auto index = std::size_t{}; index != expected.cells.size(); ++index) {
        if (std::ranges::find(layout.key_fields, index) != layout.key_fields.end())
            continue;

        if (actual_addition && std::holds_alternative<std::monostate>(expected.cells[index])) {
            const auto actual_is_valid = std::visit([](const auto &cell) {
                using Cell = std::remove_cvref_t<decltype(cell)>;
                if constexpr (std::same_as<Cell, std::monostate>)
                    return false;
                else
                    return cell.has_value();
            }, actual.cells[index]);
            if (actual_is_valid)
                return false;
            continue;
        }
        if (actual.cells[index] != expected.cells[index])
            return false;
    }
    return true;
}

Cell read_named_frame_cell(const literal::ScalarLiteral *value, const ScalarType &type,
                           bool omitted_is_null) {
    if (value == nullptr) {
        if (!omitted_is_null)
            return std::monostate{};
        return visit_cell_type(type, []<typename CellType>(std::type_identity<CellType>) -> Cell {
            return std::optional<cell_value_t<CellType>>{};
        });
    }
    return visit_cell_type(type, [&]<typename CellType>(std::type_identity<CellType>) -> Cell {
        return read_literal_cell<CellType>(*value);
    });
}

void read_named_frame_expectations(const literal::TimeSeriesLiteral &expected, const bg::Schema &schema,
                                   const NativeFrameLayout &layout, RowHistoryMatcher &history,
                                   std::size_t tick) {
    const auto entries = expected.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
            throw std::invalid_argument{"unkeyed TSF expectation must contain named row changes"};
        },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();

    for (const auto &entry : entries) {
        const auto pair = bg::details::scalar_literal::pair(
            entry, "unkeyed TSF row changes must contain a name and row value");
        const auto name = std::string{read_literal_field_name(*pair.first)};
        const auto *sentinel = literal::Sentinel::from(*pair.second);
        if (sentinel != nullptr) {
            if (*sentinel != literal::REMOVED)
                throw std::invalid_argument{"unkeyed TSF expectation uses an unsupported sentinel"};
            history.expect_remove(name, tick);
            continue;
        }

        if (history.expected_seen(name) && !history.expected_active(name))
            throw std::invalid_argument{"named row expectation reused a name"};
        const auto addition = !history.expected_seen(name);
        const auto fields = read_named_frame_fields(*pair.second, schema, layout, false);
        auto cells = std::vector<Cell>(schema.field_count());
        for (auto index = std::size_t{}; index != schema.field_count(); ++index) {
            cells[index] = read_named_frame_cell(fields[index], *layout.fields[index].cell_type, addition);
        }
        if (addition)
            history.expect_add(name, std::move(cells), tick);
        else
            history.expect_modify(name, std::move(cells), tick);
    }
}

class FrameHistoryComparison final : public testing::TickLiteralComparisonState {
  public:
    bool compare(util::buffer_reader &reader, const TimeSeriesType &,
                 const literal::TimeSeriesLiteral &expected, std::size_t tick) override {
        const auto &schema = read_schema(reader);
        const auto &layout = native_frame_layout(schema);
        if (!layout.key_fields.empty())
            return compare_keyed_frame_payload(reader, schema, expected);

        read_named_frame_expectations(expected, schema, layout, history_, tick);
        for (;;) {
            const auto header = bg::details::TickHeader::read(reader);
            if (header.operation == bg::details::TickOperation::end)
                break;
            if (header.operation != bg::details::TickOperation::add &&
                header.operation != bg::details::TickOperation::tick &&
                header.operation != bg::details::TickOperation::remove)
                throw std::invalid_argument{"invalid unkeyed TSF tick operation"};

            auto change = read_change(reader, schema, layout, header);
            if (header.operation == bg::details::TickOperation::add)
                history_.add_actual(change.id, std::move(change.cells), tick);
            else if (header.operation == bg::details::TickOperation::tick)
                history_.modify_actual(change.id, std::move(change.cells), tick);
            else
                history_.remove_actual(change.id, tick);
        }
        return history_.finish_tick();
    }

    void invalidate(std::size_t tick) override {
        history_.invalidate(tick);
        keys_by_id_.clear();
    }

    bool finish() override { return history_.finish(); }

  private:
    bool compare_keyed_frame_payload(util::buffer_reader &reader, const bg::Schema &schema,
                                     const literal::TimeSeriesLiteral &expected) {
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
            if (header.operation == bg::details::TickOperation::remove) {
                const auto found = keys_by_id_.find(actual.id);
                if (found == keys_by_id_.end())
                    return false;
                actual.key = std::move(found->second);
                keys_by_id_.erase(found);
            } else if (header.operation != bg::details::TickOperation::add) {
                const auto found = keys_by_id_.find(actual.id);
                if (found == keys_by_id_.end() || !key_values_equal(found->second, actual.key))
                    return false;
            }
            auto candidate = expected_changes.size();
            for (auto index = std::size_t{}; index != expected_changes.size(); ++index) {
                if (matched[index] || !matches_expected(actual, expected_changes[index], layout))
                    continue;
                candidate = index;
                break;
            }
            if (candidate == expected_changes.size())
                return false;
            if (header.operation == bg::details::TickOperation::add &&
                !keys_by_id_.emplace(actual.id, std::move(actual.key)).second)
                return false;
            matched[candidate] = true;
        }
    }

    RowHistoryMatcher history_;
    std::unordered_map<row_id_t, std::vector<KeyValue>> keys_by_id_;
};

template <cell_type T> class ColumnHistoryComparison final : public testing::TickLiteralComparisonState {
  public:
    bool compare(util::buffer_reader &reader, const TimeSeriesType &,
                 const literal::TimeSeriesLiteral &expected, std::size_t tick) override {
        const auto entries = expected.visit(
            [](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
                throw std::invalid_argument{"TSC expectation must contain named row changes"};
            },
            [](std::span<const literal::ScalarLiteral> values) { return values; }).value();

        for (const auto &entry : entries) {
            const auto pair = bg::details::scalar_literal::pair(
                entry, "TSC row changes must contain a name and cell value");
            const auto name = std::string{read_literal_field_name(*pair.first)};
            const auto *sentinel = literal::Sentinel::from(*pair.second);
            if (sentinel != nullptr && *sentinel == literal::REMOVED) {
                history_.expect_remove(name, tick);
                continue;
            }

            if (history_.expected_seen(name) && !history_.expected_active(name))
                throw std::invalid_argument{"named row expectation reused a name"};
            auto cells = std::vector<Cell>{read_literal_cell<T>(*pair.second)};
            if (history_.expected_seen(name))
                history_.expect_modify(name, std::move(cells), tick);
            else
                history_.expect_add(name, std::move(cells), tick);
        }

        for (;;) {
            const auto header = bg::details::TickHeader::read(reader);
            if (header.operation == bg::details::TickOperation::end)
                break;
            const auto id = reader.read_raw<row_id_t>();
            if (header.operation == bg::details::TickOperation::add)
                history_.add_actual(id, {read_cell<T>(reader)}, tick);
            else if (header.operation == bg::details::TickOperation::tick)
                history_.modify_actual(id, {read_cell<T>(reader)}, tick);
            else if (header.operation == bg::details::TickOperation::remove)
                history_.remove_actual(id, tick);
            else
                throw std::invalid_argument{"invalid TSC tick operation"};
        }
        return history_.finish_tick();
    }

    void invalidate(std::size_t tick) override { history_.invalidate(tick); }

    bool finish() override { return history_.finish(); }

  private:
    RowHistoryMatcher history_;
};

template <cell_type T> void discard_column_tick(util::buffer_reader &reader) {
    for (;;) {
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation == bg::details::TickOperation::end)
            return;
        static_cast<void>(reader.read_raw<row_id_t>());
        if (header.operation == bg::details::TickOperation::add ||
            header.operation == bg::details::TickOperation::tick) {
            discard_cell(reader, cpp_scalar_type<T>());
        } else if (header.operation != bg::details::TickOperation::remove) {
            throw std::invalid_argument{"invalid TSC tick operation"};
        }
    }
}

} // namespace

void register_frame_series_tick_comparison(const TimeSeriesType &type) {
    static_cast<void>(TimeSeriesDispatch<TimeSeriesTickOperations>::instance().add(
        type, TimeSeriesTickOperations::for_discard(&discard_frame_tick)));
    static_cast<void>(TimeSeriesDispatch<testing::TickLiteralComparison>::instance().add(
        type, testing::TickLiteralComparison{.create_state = [] {
                 return std::make_unique<FrameHistoryComparison>();
             }}));
}

void register_frame_series_tick_writer(const TimeSeriesRuntimeType &runtime) {
    auto operations = TimeSeriesTickOperations::for_write<TSF<SCHEMA>>(
        [](const In<TSF<SCHEMA>> &input, ScalarBufferWriteContext &writer, allocator &memory) {
            write_frame_tick(input, writer, memory);
        });
    static_cast<void>(TimeSeriesDispatch<TimeSeriesTickOperations>::instance().add(runtime, std::move(operations)));
}

template <typename T> void register_column_series_tick_writer(const TimeSeriesRuntimeType &runtime) {
    auto &dispatch = TimeSeriesDispatch<TimeSeriesTickOperations>::instance();
    static_cast<void>(dispatch.add(runtime.type(), TimeSeriesTickOperations::for_discard(&discard_column_tick<T>)));
    static_cast<void>(dispatch.add(
        runtime, TimeSeriesTickOperations::for_write<TSC<T>>(
                     [](const In<TSC<T>> &input, ScalarBufferWriteContext &writer, allocator &memory) {
                         write_column_tick(input, writer, memory);
                     })));
    static_cast<void>(TimeSeriesDispatch<testing::TickLiteralComparison>::instance().add(
        runtime.type(), testing::TickLiteralComparison{.create_state = [] {
                           return std::make_unique<ColumnHistoryComparison<T>>();
                       }}));
}

template BG_FRAME_EXPORT void register_column_series_tick_writer<i64>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_tick_writer<double>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_tick_writer<bool>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_tick_writer<string>(const TimeSeriesRuntimeType &runtime);
template BG_FRAME_EXPORT void register_column_series_tick_writer<symbol>(const TimeSeriesRuntimeType &runtime);

} // namespace bg::frame::details
