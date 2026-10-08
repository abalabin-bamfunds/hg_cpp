#pragma once

#include <bg/frame/tsc.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/input_data.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/output_state.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/declaration_variables.hpp>
#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_map.hpp>
#include <bg/node/output_declaration_traits.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/util/compile_time_string.hpp>
#include <bg/util/reflection.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <typeinfo>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::frame {

template <typename T> class Cell {
  public:
    Cell() = delete;
    Cell(T value) : value_{std::move(value)} {}
    Cell(std::optional<T> value) : value_{std::move(value)} {}
    Cell(std::nullopt_t) noexcept {}
    const std::optional<T> &value() const noexcept { return value_; }

  private:
    std::optional<T> value_;
};

template <typename T> class PatchCell {
  public:
    PatchCell() noexcept = default;
    PatchCell(T value) : value_{std::move(value)}, changed_{true} {}
    PatchCell(std::nullopt_t) noexcept : changed_{true} {}

    bool changed() const noexcept { return changed_; }
    const std::optional<T> &value() const noexcept { return value_; }

  private:
    std::optional<T> value_;
    bool changed_{};
};

template <template <template <typename, auto...> typename> typename Schema>
using Row = Schema<Cell>;

template <template <template <typename, auto...> typename> typename Schema>
using RowPatch = Schema<PatchCell>;

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct TSF : TimeSeriesDeclaration<TSF<Schema, Properties...>, Properties...> {};

template <util::compile_time_string... Names> struct ColumnNames {};
template <util::compile_time_string... Names> constexpr auto columns() noexcept { return ColumnNames<Names...>{}; }

class FrameType final : public ScalarType {
  public:
    explicit FrameType(const SchemaType &schema) noexcept : schema_{std::addressof(schema)} {}

    void format_name(std::string &result) const override;
    const SchemaType &schema_type() const noexcept { return *schema_; }
    bool is_resolved() const noexcept override { return schema_->is_resolved(); }
    const Type &resolve(const TypeMap &type_map) const override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const SchemaType *schema_;
};

class FrameSeriesType final : public FixedItemTimeSeriesType {
  public:
    FrameSeriesType(const SchemaType &schema, GuaranteeParameter guarantee);

    void format_name(std::string &result) const override;
    const SchemaType &schema_type() const noexcept { return *schema_; }
    bool is_resolved() const noexcept override { return schema_->is_resolved() && guarantee().is_resolved(); }
    std::size_t item_count() const noexcept override { return schema_->field_count(); }
    const TimeSeriesType &item_type(std::size_t index) const override;
    const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;
    const Type &resolve(const TypeMap &type_map) const override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const SchemaType *schema_;
    std::vector<const TimeSeriesType *> item_types_;
};

const FrameType &frame_type(const SchemaType &schema);
const FrameSeriesType &frame_series_type(const SchemaType &schema, GuaranteeParameter guarantee);

template <template <template <typename, auto...> typename> typename Schema>
const SchemaType &frame_schema_type() {
    if constexpr (bg::details::schema_variable_template<Schema>) {
        return bg::details::schema_variable_instance<Schema>();
    } else {
        return bg::details::cpp_hierarchical_scalar_schema<Schema>();
    }
}

template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesType &frame_time_series_type(TimeSeriesGuarantee guarantee);
template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesRuntimeType &frame_time_series_runtime_type(const TimeSeriesType &type);

template <template <template <typename, auto...> typename> typename Schema> class Frame;
template <template <template <typename, auto...> typename> typename Schema> class FrameView;

namespace details {
using frame_series_runtime_factory = std::unique_ptr<TimeSeriesRuntimeType> (*)(const TimeSeriesType &);
const TimeSeriesRuntimeType &canonical_frame_series_runtime_type(const std::type_info &identity,
                                                                 const TimeSeriesType &type,
                                                                 frame_series_runtime_factory factory);
}

template <template <template <typename, auto...> typename> typename Schema> struct FrameStorage {
    static constexpr bool supports_byte_copy_initialization = true;
    static constexpr auto field_count = util::aggregate_member_count<Schema<ColumnStorage>>;
    Schema<ColumnStorage> columns;
    RowSet rows{};
    const util::index<> *added{};
    const util::index<> *removed{};
    const util::index<> *changed{};
    std::array<const util::index<> *, field_count> modified{};
    const util::index<field_count> *changed_columns{};
    EvaluationTime change_time{MIN_DT};
    bool valid{};
    std::size_t size{};
};

template <template <template <typename, auto...> typename> typename Schema>
class FrameSeriesRuntimeType : public TimeSeriesRuntimeType {
  public:
    virtual const FrameStorage<Schema> &input_storage(const void *value) const noexcept = 0;

  protected:
    FrameSeriesRuntimeType(const TimeSeriesType &type, std::size_t size, std::size_t alignment,
                           bool uses_variable_memory, bool requires_finalization,
                           bool supports_byte_copy_initialization) noexcept
        : TimeSeriesRuntimeType{type, size, alignment, uses_variable_memory, requires_finalization,
                                supports_byte_copy_initialization} {}
};

namespace details {
template <template <template <typename, auto...> typename> typename Schema> struct FrameInputDescriptor {
    const FrameStorage<Schema> *storage{};
    bool valid{};
    bool modified{};
    EvaluationTime last_updated{MIN_DT};
};
} // namespace details

namespace details {

template <typename T> struct frame_column_storage {
    using scalar_type = void;
};

template <typename T> struct frame_schema_cell;
template <typename T> struct frame_schema_cell<Column<T>> {
    using type = T;
};

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema>
using frame_schema_cell_t = typename frame_schema_cell<std::remove_cvref_t<decltype(
    std::get<Index>(util::aggregate_members(std::declval<Schema<Column> &>())))>>::type;

template <std::size_t Index, util::compile_time_string... Names>
consteval auto frame_column_name(ColumnNames<Names...>) {
    constexpr auto names = std::array{Names...};
    return names[Index];
}
template <typename T> struct frame_column_storage<ColumnStorage<T>> {
    using scalar_type = Column<T>;
    using cell_type = T;
};

template <template <template <typename, auto...> typename> typename Schema, typename Storage, std::size_t... Index>
auto frame_columns_view(const Storage &storage, std::index_sequence<Index...>) {
    auto result = Schema<ColumnView>{};
    auto source = util::aggregate_members(storage);
    auto destination = util::aggregate_members(result);
    ((std::get<Index>(destination) = scalar_storage_traits<typename frame_column_storage<
          std::remove_cvref_t<decltype(std::get<Index>(source))>>::scalar_type>::read(std::get<Index>(source))),
     ...);
    return result;
}

template <util::compile_time_string Name, template <template <typename, auto...> typename> typename Schema>
consteval std::size_t frame_column_index() {
    constexpr auto names = util::aggregate_member_names<Schema<Column>>;
    for (auto index = std::size_t{}; index != names.size(); ++index) {
        if (names[index] == Name.view())
            return index;
    }
    return names.size();
}

} // namespace details

template <template <template <typename, auto...> typename> typename Schema> class FrameView {
  public:
    using columns_type = Schema<ColumnView>;

    FrameView() = default;
    FrameView(columns_type columns, std::size_t size) : columns{std::move(columns)}, size_{size} {}
    explicit FrameView(const Frame<Schema> &frame);

    std::size_t size() const noexcept { return size_; }
    columns_type columns;

  private:
    std::size_t size_{};
};

template <template <template <typename, auto...> typename> typename Schema> class Frame {
  public:
    using columns_type = Schema<Column>;

    Frame() = default;
    explicit Frame(columns_type columns) : columns_{std::move(columns)}, size_{column_size(columns_)} {
        validate_column_sizes();
        bind_storage();
    }
    Frame(columns_type columns, std::size_t size) : columns_{std::move(columns)}, size_{size} {
        validate_column_sizes();
        bind_storage();
    }
    Frame(const Frame &other) : columns_{other.columns_}, size_{other.size_} { bind_storage(); }
    Frame(Frame &&other) noexcept : columns_{std::move(other.columns_)}, size_{std::exchange(other.size_, 0)} {
        bind_storage();
        other.storage_ = {};
    }
    Frame &operator=(const Frame &other) {
        if (this != &other) {
            columns_ = other.columns_;
            size_ = other.size_;
            bind_storage();
        }
        return *this;
    }
    Frame &operator=(Frame &&other) noexcept {
        if (this != &other) {
            columns_ = std::move(other.columns_);
            size_ = std::exchange(other.size_, 0);
            bind_storage();
            other.storage_ = {};
        }
        return *this;
    }

    std::size_t size() const noexcept { return size_; }
    const columns_type &columns() const noexcept { return columns_; }
    const FrameStorage<Schema> &scalar_storage() const noexcept { return storage_; }

  private:
    static std::size_t column_size(const columns_type &columns) {
        if constexpr (util::aggregate_member_count<columns_type> == 0)
            return 0;
        else {
            const auto members = util::aggregate_members(columns);
            return std::get<0>(members).size();
        }
    }
    void validate_column_sizes() const {
        if constexpr (util::aggregate_member_count<columns_type> != 0) {
            const auto members = util::aggregate_members(columns_);
            std::apply(
                [&](const auto &first, const auto &...rest) {
                    if (first.size() != size_ || ((rest.size() != size_) || ...))
                        throw std::invalid_argument{"frame columns have different lengths"};
                },
                members);
        }
    }
    void bind_storage() {
        auto destination = util::aggregate_members(storage_.columns);
        auto source = util::aggregate_members(columns_);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((std::get<Index>(destination) = std::get<Index>(source).storage()), ...);
        }(std::make_index_sequence<util::aggregate_member_count<columns_type>>{});
        storage_.size = size_;
    }

    columns_type columns_;
    std::size_t size_{};
    FrameStorage<Schema> storage_;
};

template <template <template <typename, auto...> typename> typename Schema>
FrameView<Schema>::FrameView(const Frame<Schema> &frame) : size_{frame.size()} {
    auto source = util::aggregate_members(frame.columns());
    auto destination = util::aggregate_members(columns);
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((std::get<Index>(destination) = std::get<Index>(source).view()), ...);
    }(std::make_index_sequence<util::aggregate_member_count<typename Frame<Schema>::columns_type>>{});
}

template <template <template <typename, auto...> typename> typename Schema>
class NativeFrame {
  public:
    using fields_type = Schema<NativeColumnData>;
    template <typename, auto...> using child_state = TimeSeriesState;
    using child_states_type = Schema<child_state>;
    using row_type = Row<Schema>;
    using patch_type = RowPatch<Schema>;

    NativeFrame() noexcept = default;
    NativeFrame(const NativeFrame &) = delete;
    static constexpr bool supports_byte_copy_initialization = true;

    std::size_t size() const noexcept { return rows_.size(); }
    bool valid() const noexcept { return valid_; }
    bool contains(row_id_t id) const noexcept { return rows_.contains(id); }
    row_id_t id_at(std::size_t position) const { return rows_.id_at(position); }
    const void *row_domain() const noexcept { return rows_.identity(); }
    const util::index<> &added() const noexcept { return rows_.added(); }
    const util::index<> &removed() const noexcept { return rows_.removed(); }
    EvaluationTime change_time() const noexcept { return rows_.change_time(); }
    const FrameStorage<Schema> &scalar_storage() const noexcept { return scalar_; }
    const NativeRows &rows() const noexcept { return rows_; }
    const child_states_type &child_states() const noexcept { return child_states_; }
    const auto &changed_columns() const noexcept { return changed_columns_; }

    template <std::size_t Index> const auto &column() const noexcept {
        return std::get<Index>(util::aggregate_members(fields_));
    }
    template <std::size_t Index> auto cell(row_id_t id) const {
        return column<Index>().cell(rows_, id);
    }
    template <std::size_t Index> const util::index<> &modified() const noexcept {
        return column<Index>().modified();
    }

    row_id_t add(const row_type &row, EvaluationTime time, allocator memory) {
        begin_cycle(time);
        const auto id = rows_.add(time, memory);
        const auto slot = rows_.slot_of(id);
        auto rollback = util::scope_exit{[&]() noexcept {
            for_each_field([&](auto &field) { field.erase_slot(slot, id, memory); });
            changed_.reset(id);
            rows_.rollback_add(id);
        }};
        changed_.set(id, memory.memory_resource());
        const auto values = util::aggregate_members(row);
        for_each_field_index([&]<std::size_t Index>(auto &field) {
            field.add(rows_, id, std::get<Index>(values).value(), memory);
        });
        publish_all_columns(time);
        valid_ = true;
        bind_scalar_storage();
        rollback.release();
        return id;
    }

    bool set(row_id_t id, const patch_type &patch, EvaluationTime time, allocator memory) {
        begin_cycle(time);
        if (!rows_.contains(id))
            throw std::out_of_range{"frame row ID"};
        const auto patches = util::aggregate_members(patch);
        const auto has_change = std::apply(
            [](const auto &...value) { return (value.changed() || ...); }, patches);
        if (!has_change)
            return false;
        const auto was_changed = changed_.contains(id);
        if (!was_changed)
            changed_.set(id, memory.memory_resource());
        auto remove_empty_change = util::scope_exit{[&]() noexcept {
            if (!was_changed && !any_column_modified(id))
                changed_.reset(id);
        }};
        for_each_field_index([&]<std::size_t Index>(auto &field) {
            const auto &value = std::get<Index>(patches);
            if (value.changed()) {
                field.set(rows_, id, value.value(), memory);
                publish_column<Index>(time);
            }
        });
        valid_ = true;
        bind_scalar_storage();
        remove_empty_change.release();
        return true;
    }

    bool erase(row_id_t id, EvaluationTime time, allocator memory) {
        begin_cycle(time);
        if (!rows_.contains(id))
            return false;
        const auto slot = rows_.slot_of(id);
        if (!rows_.erase(id, time, memory))
            return false;
        for_each_field([&](auto &field) { field.erase_slot(slot, id, memory); });
        publish_all_columns(time);
        valid_ = true;
        changed_.reset(id);
        bind_scalar_storage();
        return true;
    }

    void clear(EvaluationTime time, allocator memory) {
        begin_cycle(time);
        const auto was_valid = valid_;
        auto removed_rows = false;
        auto slot_iterator = rows_.live_slots().begin();
        while (slot_iterator != rows_.live_slots().end()) {
            const auto slot = *slot_iterator;
            ++slot_iterator;
            const auto id = rows_.id_at_slot(slot);
            if (!rows_.erase(id, time, memory))
                std::terminate();
            for_each_field([&](auto &field) { field.erase_slot(slot, id, memory); });
            changed_.reset(id);
            removed_rows = true;
        }
        if (!was_valid || removed_rows)
            publish_all_columns(time);
        valid_ = true;
        bind_scalar_storage();
    }

    void invalidate(EvaluationTime time, allocator memory) {
        clear(time, memory);
        valid_ = false;
        scalar_.valid = false;
    }

    template <typename Function> void for_each_changed(Function &&function) const {
        for (auto id : changed_)
            std::invoke(function, id);
    }

    template <typename Function> void for_each_removed(Function &&function) const {
        for (auto id : rows_.removed())
            std::invoke(function, id);
    }

    void release(allocator memory) noexcept {
        for_each_field([&](auto &field) { field.release(memory); });
        rows_.release(memory);
        changed_.release(memory.memory_resource());
        changed_columns_.clear();
        scalar_ = {};
        child_states_ = {};
        valid_ = false;
    }

  private:
    void begin_cycle(EvaluationTime time) {
        if (!rows_.reset_changes(time))
            return;
        changed_.clear();
        changed_columns_.clear();
        for_each_field([&](auto &field) { field.reset_changes(); });
    }

    template <std::size_t Index> void publish_column(EvaluationTime time) {
        std::get<Index>(util::aggregate_members(child_states_)).publish(time);
        changed_columns_.set(Index);
    }

    template <std::size_t... Index> void publish_all_columns(EvaluationTime time, std::index_sequence<Index...>) {
        (publish_column<Index>(time), ...);
    }

    void publish_all_columns(EvaluationTime time) {
        publish_all_columns(time, std::make_index_sequence<util::aggregate_member_count<fields_type>>{});
    }

    template <typename Function> void for_each_field(Function &&function) {
        std::apply(
            [&](auto &...field) { (std::invoke(function, field), ...); },
            util::aggregate_members(fields_));
    }

    void bind_scalar_storage() noexcept {
        auto destination = util::aggregate_members(scalar_.columns);
        auto source = util::aggregate_members(fields_);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((std::get<Index>(destination) = ColumnStorage{std::get<Index>(source), rows_}), ...);
        }(std::make_index_sequence<util::aggregate_member_count<fields_type>>{});
        scalar_.rows = RowSet::native(rows_);
        scalar_.added = std::addressof(rows_.added());
        scalar_.removed = std::addressof(rows_.removed());
        scalar_.changed = std::addressof(changed_);
        scalar_.changed_columns = std::addressof(changed_columns_);
        scalar_.change_time = rows_.change_time();
        scalar_.valid = valid_;
        auto source_fields = util::aggregate_members(fields_);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((scalar_.modified[Index] = std::addressof(std::get<Index>(source_fields).modified())), ...);
        }(std::make_index_sequence<util::aggregate_member_count<fields_type>>{});
        scalar_.size = rows_.size();
    }

    template <typename Function> void for_each_field_index(Function &&function) {
        auto members = util::aggregate_members(fields_);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (function.template operator()<Index>(std::get<Index>(members)), ...);
        }(std::make_index_sequence<util::aggregate_member_count<fields_type>>{});
    }

    bool any_column_modified(row_id_t id) const noexcept {
        const auto members = util::aggregate_members(fields_);
        return std::apply([&](const auto &...field) { return (field.modified().contains(id) || ...); }, members);
    }

    NativeRows rows_;
    fields_type fields_;
    child_states_type child_states_;
    util::index<> changed_;
    util::index<util::aggregate_member_count<fields_type>> changed_columns_;
    FrameStorage<Schema> scalar_;
    bool valid_{};
};

template <template <template <typename, auto...> typename> typename Schema> struct FrameViewOutputStorage {
    using fields_type = Schema<ColumnStorage>;
    enum class Operation { none, projection, filter };
    template <typename, auto...> using child_state = TimeSeriesState;
    using child_states_type = Schema<child_state>;
    template <typename, auto...> using row_index = util::index<>;
    using modified_rows_type = Schema<row_index>;

    struct ColumnMapping {
        const void *native{};
        const NativeRows *rows{};
        const void *snapshot{};
        const void *visible_context{};
        const void *visible_identity{};
        const void *coverage_context{};
        const void *coverage_identity{};

        friend bool operator==(const ColumnMapping &, const ColumnMapping &) = default;
    };

    child_states_type child_states;
    SelectedRows previous_rows;
    SelectedRows selected_rows;
    FrameStorage<Schema> scalar;
    util::index<util::aggregate_member_count<fields_type>> changed_columns;
    util::index<> added_rows;
    util::index<> removed_rows;
    util::index<> changed_rows;
    util::index<> predicate_rows;
    modified_rows_type modified_rows;
    std::array<ColumnMapping, util::aggregate_member_count<fields_type>> mapping{};
    EvaluationTime delta_time{MIN_DT};
    Operation operation{Operation::none};
    bool valid{};

    template <template <template <typename, auto...> typename> typename SourceSchema,
              util::compile_time_string... Names>
    bool bind_projection(const FrameStorage<SourceSchema> &input, bool source_modified,
                         EvaluationTime source_last_updated, EvaluationTime time, allocator memory,
                         ColumnNames<Names...>) {
        if (valid && operation == Operation::filter)
            throw std::logic_error{"a TSF view output cannot switch from filtering to projection"};

        constexpr auto field_count = util::aggregate_member_count<fields_type>;
        static_assert(sizeof...(Names) == field_count, "projection must name one source column per target column");
        static_assert(((details::frame_column_index<Names, SourceSchema>() <
                        util::aggregate_member_count<SourceSchema<Column>>) && ...),
                      "projection names an unknown source column");
        constexpr auto source_indices =
            std::array<std::size_t, field_count>{details::frame_column_index<Names, SourceSchema>()...};

        auto source_fields = util::aggregate_members(input.columns);
        auto target_fields = util::aggregate_members(scalar.columns);
        auto target_states = util::aggregate_members(child_states);
        auto target_modified = util::aggregate_members(modified_rows);
        auto next_mapping = std::array<ColumnMapping, field_count>{};
        const auto new_cycle = !valid || time != delta_time;
        const auto previous_row_set = previous_rows.source();
        const auto row_domain_changed = previous_row_set.identity != input.rows.identity;
        const auto row_set_rebound = !valid || previous_row_set.context != input.rows.context || row_domain_changed;
        auto mapping_changed = std::array<bool, field_count>{};

        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (([&] {
                 constexpr auto source_index = source_indices[Index];
                 using TargetColumn = std::remove_cvref_t<decltype(std::get<Index>(target_fields))>;
                 using SourceColumn = std::remove_cvref_t<decltype(std::get<source_index>(source_fields))>;
                 static_assert(std::same_as<typename details::frame_column_storage<TargetColumn>::cell_type,
                                            typename details::frame_column_storage<SourceColumn>::cell_type>,
                               "projection source and target column types must match");
                 const auto &column = std::get<source_index>(source_fields);
                 if (column.native == nullptr || column.rows == nullptr ||
                     column.rows->identity() != input.rows.identity)
                     throw std::invalid_argument{"projection column has a different physical row domain"};
                 const auto identity = ColumnMapping{column.native,
                                                     column.rows,
                                                     column.snapshot,
                                                     column.visible_rows.context,
                                                     column.visible_rows.identity,
                                                     column.coverage.context,
                                                     column.coverage.identity};
                 next_mapping[Index] = identity;
                 mapping_changed[Index] = !valid || mapping[Index] != identity;
                 const auto exact_coverage = column.coverage.context == input.rows.context &&
                                             column.coverage.identity == input.rows.identity;
                 if (!exact_coverage) {
                     input.rows.each([&](row_id_t id) {
                         if (!column.coverage.has(id) || !column.rows->contains(id))
                             throw std::invalid_argument{"projection column does not cover every output row"};
                     });
                 }
             }()),
             ...);
        }(std::make_index_sequence<field_count>{});

        const auto sampled_rebuild = source_modified && input.change_time != source_last_updated;
        mapping = next_mapping;

        if (new_cycle) {
            added_rows.clear();
            removed_rows.clear();
            changed_rows.clear();
            changed_columns.clear();
            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                (std::get<Index>(target_modified).clear(), ...);
            }(std::make_index_sequence<field_count>{});
            delta_time = time;
        }

        const auto add_membership = [&](row_id_t id) {
            if (removed_rows.reset(id))
                return;
            if (!added_rows.contains(id))
                added_rows.set(id, memory.memory_resource());
            changed_rows.set(id, memory.memory_resource());
        };
        const auto remove_membership = [&](row_id_t id) {
            if (added_rows.reset(id)) {
                changed_rows.reset(id);
            } else {
                removed_rows.set(id, memory.memory_resource());
            }
            changed_rows.reset(id);
            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                (std::get<Index>(target_modified).reset(id), ...);
            }(std::make_index_sequence<field_count>{});
        };

        if (row_set_rebound || !new_cycle || sampled_rebuild) {
            if (row_domain_changed) {
                previous_rows.for_each_selected([&](row_id_t id) { removed_rows.set(id, memory.memory_resource()); });
                previous_rows.clear();
                input.rows.each([&](row_id_t id) {
                    added_rows.set(id, memory.memory_resource());
                    changed_rows.set(id, memory.memory_resource());
                });
            } else {
                previous_rows.for_each_selected([&](row_id_t id) {
                    if (!input.rows.has(id))
                        remove_membership(id);
                });
                input.rows.each([&](row_id_t id) {
                    if (previous_rows.contains(id))
                        removed_rows.reset(id);
                    else
                        add_membership(id);
                });
                for (auto id : removed_rows)
                    if (!input.rows.has(id))
                        previous_rows.exclude(id);
            }
            previous_rows.bind(input.rows);
            input.rows.each([&](row_id_t id) {
                if (!previous_rows.contains(id))
                    previous_rows.include(id, memory);
            });
        } else if (source_modified && input.change_time == source_last_updated) {
            if (input.removed != nullptr) {
                for (auto id : *input.removed)
                    if (!input.rows.has(id) && previous_rows.contains(id))
                        remove_membership(id);
            }
            for (auto id : removed_rows)
                previous_rows.exclude(id);
            if (input.added != nullptr) {
                for (auto id : *input.added)
                    if (input.rows.has(id) && !previous_rows.contains(id))
                        add_membership(id);
            }
            for (auto id : added_rows)
                previous_rows.include(id, memory);
        }

        auto any_change = !valid || !added_rows.empty() || !removed_rows.empty();
        const auto input_changes_current = source_modified && input.change_time == source_last_updated;
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (([&] {
                 constexpr auto source_index = source_indices[Index];
                 const auto &column = std::get<source_index>(source_fields);
                 auto &output_column = std::get<Index>(target_fields);
                 auto &output_modified = std::get<Index>(target_modified);
                 const auto *source_modified_rows = input_changes_current ? input.modified[source_index] : nullptr;
                 if (mapping_changed[Index] || sampled_rebuild) {
                     input.rows.each([&](row_id_t id) {
                         if (!added_rows.contains(id))
                             output_modified.set(id, memory.memory_resource());
                     });
                 } else if (source_modified_rows != nullptr) {
                     for (auto id : *source_modified_rows) {
                         if (input.rows.has(id) && !added_rows.contains(id)) {
                             output_modified.set(id, memory.memory_resource());
                             changed_rows.set(id, memory.memory_resource());
                         }
                     }
                 }
                 if (!output_modified.empty()) {
                     if (mapping_changed[Index])
                         for (auto id : output_modified)
                             changed_rows.set(id, memory.memory_resource());
                     any_change = true;
                 }
                 scalar.modified[Index] = std::addressof(output_modified);
                 if (mapping_changed[Index] || !output_modified.empty()) {
                     changed_columns.set(Index);
                     std::get<Index>(target_states).publish(time);
                     any_change = true;
                 }

                 output_column = column;
                 output_column.visible_rows = input.rows;
                 output_column.coverage = input.rows;
                 output_column.added = std::addressof(added_rows);
                 output_column.modified = scalar.modified[Index];
                 output_column.removed = std::addressof(removed_rows);
                 output_column.change_time = time;
             }()),
             ...);
        }(std::make_index_sequence<field_count>{});

        if (!added_rows.empty() || !removed_rows.empty()) {
            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                (([&] {
                     changed_columns.set(Index);
                     std::get<Index>(target_states).publish(time);
                 }()),
                 ...);
            }(std::make_index_sequence<field_count>{});
        }

        scalar.rows = input.rows;
        scalar.added = std::addressof(added_rows);
        scalar.removed = std::addressof(removed_rows);
        scalar.changed = std::addressof(changed_rows);
        scalar.changed_columns = std::addressof(changed_columns);
        scalar.change_time = time;
        scalar.valid = true;
        scalar.size = input.rows.count();
        valid = true;
        operation = Operation::projection;
        return any_change;
    }

    template <util::compile_time_string Name, template <template <typename, auto...> typename> typename SourceSchema,
              typename Predicate>
    bool bind_filter(const FrameStorage<SourceSchema> &input, bool source_modified,
                     EvaluationTime source_last_updated, EvaluationTime time, allocator memory,
                     Predicate &&predicate) {
        if (valid && operation == Operation::projection)
            throw std::logic_error{"a TSF view output cannot switch from projection to filtering"};

        constexpr auto field_count = util::aggregate_member_count<fields_type>;
        static_assert(field_count == util::aggregate_member_count<SourceSchema<ColumnStorage>>,
                      "filtered frame must retain its source schema");
        constexpr auto predicate_index = details::frame_column_index<Name, SourceSchema>();
        static_assert(predicate_index < field_count, "filter names an unknown predicate column");
        static_assert(cell_type<details::frame_schema_cell_t<predicate_index, SourceSchema>>,
                      "filter predicate column must have a supported numeric cell type");

        auto source_fields = util::aggregate_members(input.columns);
        auto target_fields = util::aggregate_members(scalar.columns);
        auto target_states = util::aggregate_members(child_states);
        auto target_modified = util::aggregate_members(modified_rows);
        auto next_mapping = std::array<ColumnMapping, field_count>{};
        auto mapping_changed = std::array<bool, field_count>{};

        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (([&] {
                 const auto &column = std::get<Index>(source_fields);
                 if (column.native == nullptr || column.rows == nullptr ||
                     column.rows->identity() != input.rows.identity)
                     throw std::invalid_argument{"filter column has a different physical row domain"};
                 const auto exact_coverage = column.coverage.context == input.rows.context &&
                                             column.coverage.identity == input.rows.identity;
                 if (!exact_coverage)
                     input.rows.each([&](row_id_t id) {
                         if (!column.coverage.has(id) || !column.rows->contains(id))
                             throw std::invalid_argument{"filter column does not cover every source row"};
                     });

                 next_mapping[Index] = ColumnMapping{column.native,
                                                     column.rows,
                                                     column.snapshot,
                                                     column.visible_rows.context,
                                                     column.visible_rows.identity,
                                                     column.coverage.context,
                                                     column.coverage.identity};
                 mapping_changed[Index] = !valid || mapping[Index] != next_mapping[Index];
             }()),
             ...);
        }(std::make_index_sequence<field_count>{});

        const auto previous_row_set = previous_rows.source();
        const auto row_domain_changed = previous_row_set.identity != nullptr &&
                                        previous_row_set.identity != input.rows.identity;
        const auto row_set_rebound = !valid || previous_row_set.context != input.rows.context;
        const auto new_cycle = !valid || time != delta_time;
        const auto input_delta_current = source_modified && input.change_time == source_last_updated;
        const auto rebuild = !valid || row_set_rebound || row_domain_changed || mapping_changed[predicate_index] ||
                             (source_modified && !input_delta_current);

        mapping = next_mapping;
        if (new_cycle) {
            added_rows.clear();
            removed_rows.clear();
            changed_rows.clear();
            changed_columns.clear();
            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                (std::get<Index>(target_modified).clear(), ...);
            }(std::make_index_sequence<field_count>{});
            delta_time = time;
        }
        predicate_rows.clear();

        const auto remove_selected = [&](row_id_t id, bool preserve_domain_removal) {
            if (!selected_rows.exclude(id))
                return;
            if (preserve_domain_removal || !added_rows.reset(id))
                removed_rows.set(id, memory.memory_resource());
            changed_rows.reset(id);
            [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                (std::get<Index>(target_modified).reset(id), ...);
            }(std::make_index_sequence<field_count>{});
        };
        const auto add_selected = [&](row_id_t id, bool preserve_domain_addition) {
            if (!selected_rows.include(id, memory))
                return;
            if (preserve_domain_addition || !removed_rows.reset(id))
                added_rows.set(id, memory.memory_resource());
            changed_rows.set(id, memory.memory_resource());
        };

        if (row_domain_changed) {
            selected_rows.for_each_selected([&](row_id_t id) {
                removed_rows.set(id, memory.memory_resource());
                changed_rows.reset(id);
                [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                    (std::get<Index>(target_modified).reset(id), ...);
                }(std::make_index_sequence<field_count>{});
            });
            selected_rows.clear();
            previous_rows.clear();
            input.rows.each([&](row_id_t id) {
                previous_rows.include(id, memory);
                predicate_rows.set(id, memory.memory_resource());
            });
        } else if (row_set_rebound || (source_modified && !input_delta_current)) {
            previous_rows.for_each_selected([&](row_id_t id) {
                if (!input.rows.has(id))
                    predicate_rows.set(id, memory.memory_resource());
            });
            for (auto id : predicate_rows) {
                remove_selected(id, false);
                previous_rows.exclude(id);
            }
            predicate_rows.clear();
            input.rows.each([&](row_id_t id) {
                if (!previous_rows.contains(id))
                    predicate_rows.set(id, memory.memory_resource());
                previous_rows.include(id, memory);
            });
        } else if (input_delta_current) {
            if (input.removed != nullptr) {
                for (auto id : *input.removed) {
                    if (!input.rows.has(id) && previous_rows.contains(id)) {
                        remove_selected(id, false);
                        previous_rows.exclude(id);
                    }
                }
            }
            if (input.added != nullptr) {
                for (auto id : *input.added) {
                    if (input.rows.has(id) && !previous_rows.contains(id)) {
                        predicate_rows.set(id, memory.memory_resource());
                        previous_rows.include(id, memory);
                    }
                }
            }
        }

        if (rebuild) {
            input.rows.each([&](row_id_t id) { predicate_rows.set(id, memory.memory_resource()); });
        } else if (input_delta_current && input.modified[predicate_index] != nullptr) {
            for (auto id : *input.modified[predicate_index])
                if (input.rows.has(id))
                    predicate_rows.set(id, memory.memory_resource());
        }

        selected_rows.bind(input.rows);
        for (auto id : predicate_rows) {
            const auto qualifies = std::invoke(predicate, id);
            if (row_domain_changed) {
                if (qualifies)
                    add_selected(id, true);
                continue;
            }
            if (qualifies)
                add_selected(id, false);
            else
                remove_selected(id, false);
        }
        previous_rows.bind(input.rows);

        const auto input_changes_current = input_delta_current;
        auto any_change = !valid || !added_rows.empty() || !removed_rows.empty();
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (([&] {
                 const auto &column = std::get<Index>(source_fields);
                 auto &output_column = std::get<Index>(target_fields);
                 auto &output_modified = std::get<Index>(target_modified);
                 const auto *source_modified_rows = input_changes_current ? input.modified[Index] : nullptr;
                 if ((mapping_changed[Index] || (source_modified && !input_delta_current))) {
                     input.rows.each([&](row_id_t id) {
                         if (selected_rows.contains(id) && !added_rows.contains(id))
                             output_modified.set(id, memory.memory_resource());
                     });
                 } else if (source_modified_rows != nullptr) {
                     for (auto id : *source_modified_rows) {
                         if (selected_rows.contains(id) && !added_rows.contains(id))
                             output_modified.set(id, memory.memory_resource());
                     }
                 }
                 if (!output_modified.empty()) {
                     for (auto id : output_modified)
                         changed_rows.set(id, memory.memory_resource());
                     any_change = true;
                 }
                 scalar.modified[Index] = std::addressof(output_modified);
                 if (mapping_changed[Index] || !output_modified.empty() || !added_rows.empty() ||
                     !removed_rows.empty()) {
                     changed_columns.set(Index);
                     std::get<Index>(target_states).publish(time);
                     any_change = true;
                 }

                 output_column = column;
                 output_column.visible_rows = selected_rows.rows();
                 output_column.coverage = selected_rows.rows();
                 output_column.added = std::addressof(added_rows);
                 output_column.modified = scalar.modified[Index];
                 output_column.removed = std::addressof(removed_rows);
                 output_column.change_time = time;
             }()),
             ...);
        }(std::make_index_sequence<field_count>{});

        scalar.rows = selected_rows.rows();
        scalar.added = std::addressof(added_rows);
        scalar.removed = std::addressof(removed_rows);
        scalar.changed = std::addressof(changed_rows);
        scalar.changed_columns = std::addressof(changed_columns);
        scalar.change_time = time;
        scalar.valid = true;
        scalar.size = selected_rows.size();
        valid = true;
        operation = Operation::filter;
        return any_change;
    }

    void invalidate() noexcept {
        previous_rows.clear();
        previous_rows.bind({});
        selected_rows.clear();
        selected_rows.bind({});
        added_rows.clear();
        removed_rows.clear();
        changed_rows.clear();
        predicate_rows.clear();
        changed_columns.clear();
        auto modified = util::aggregate_members(modified_rows);
        std::apply([](auto &...field) { (field.clear(), ...); }, modified);
        scalar = {};
        mapping = {};
        operation = Operation::none;
        valid = false;
    }

    void release(allocator memory) noexcept {
        previous_rows.release(memory);
        selected_rows.release(memory);
        added_rows.release(memory.memory_resource());
        removed_rows.release(memory.memory_resource());
        changed_rows.release(memory.memory_resource());
        predicate_rows.release(memory.memory_resource());
        auto modified = util::aggregate_members(modified_rows);
        std::apply([&](auto &...field) { (field.release(memory.memory_resource()), ...); }, modified);
        changed_columns.clear();
        scalar = {};
        operation = Operation::none;
        valid = false;
    }
};

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema, auto... Properties>
auto select_column(wp<TSF<Schema, Properties...>> source) {
    using Cell = details::frame_schema_cell_t<Index, Schema>;
    using ColumnSeries = TSC<Cell>;
    const auto child = WiringContext::current().wiring().output_item(
        static_cast<WiringPoint>(source), static_cast<item_id_t>(Index), ColumnSeries::type());
    return child.template as<ColumnSeries>();
}

template <util::compile_time_string Name,
          template <template <typename, auto...> typename> typename Schema, auto... Properties>
auto select_column(wp<TSF<Schema, Properties...>> source) {
    constexpr auto index = details::frame_column_index<Name, Schema>();
    static_assert(index < util::aggregate_member_count<Schema<Column>>, "unknown frame column name");
    return select_column<index>(source);
}

namespace details {

template <template <template <typename, auto...> typename> typename Schema>
class NativeFrameSeriesRuntimeType final : public bg::frame::FrameSeriesRuntimeType<Schema> {
  public:
    using ItemLayout = TimeSeriesRuntimeType::ItemLayout;
    using ConstItem = TimeSeriesRuntimeType::ConstItem;
    using MutableItem = TimeSeriesRuntimeType::MutableItem;
    using ItemIdFunction = TimeSeriesRuntimeType::ItemIdFunction;
    using ModifiedItemFunction = TimeSeriesRuntimeType::ModifiedItemFunction;

    explicit NativeFrameSeriesRuntimeType(const TimeSeriesType &type)
        : bg::frame::FrameSeriesRuntimeType<Schema>{type, sizeof(NativeFrame<Schema>), alignof(NativeFrame<Schema>),
                                                    true, true, true} {
        const auto sample = NativeFrame<Schema>{};
        append_item_layouts(sample, std::make_index_sequence<util::aggregate_member_count<Schema<ColumnStorage>>>{});
    }

    ItemLayout item_layout(item_id_t id) const override { return fields_.at(id); }
    std::size_t item_count(const void *) const override { return fields_.size(); }
    ConstItem item(const void *value, item_id_t id) const override {
        const auto &field = fields_.at(id);
        const auto *bytes = static_cast<const std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<const TimeSeriesState *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }
    MutableItem item(void *value, item_id_t id) const override {
        const auto &field = fields_.at(id);
        auto *bytes = static_cast<std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<TimeSeriesState *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }
    void for_each_item_id(const void *, void *context, ItemIdFunction function) const override {
        for (auto id = item_id_t{}; id < fields_.size(); ++id)
            function(context, id);
    }
    bool item_updated(const TimeSeriesState &last_updated, const void *value, item_id_t id) const noexcept override {
        const auto &frame = *static_cast<const NativeFrame<Schema> *>(value);
        return last_updated.valid() && frame.change_time() == last_updated.time() &&
               frame.changed_columns().contains(id);
    }
    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &frame = *static_cast<const NativeFrame<Schema> *>(value);
        if (!last_updated.valid() || frame.change_time() != last_updated.time())
            return;
        for (auto id : frame.changed_columns())
            function(context, id);
    }
    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &frame = *static_cast<const NativeFrame<Schema> *>(value);
        const auto *bytes = static_cast<const std::byte *>(value);
        const auto publish = [&](item_id_t id) {
            const auto &field = fields_[id];
            function(context, {id, &field.runtime_type,
                               reinterpret_cast<const TimeSeriesState *>(bytes + field.last_updated_offset),
                               bytes + field.value_offset});
        };
        if (!last_updated.valid()) {
            for (auto id = item_id_t{}; id < fields_.size(); ++id)
                publish(id);
            return;
        }
        if (frame.change_time() != evaluation_time)
            return;
        for (auto id : frame.changed_columns()) {
            publish(id);
        }
    }
    bool collection_valid(const void *value) const noexcept override {
        return static_cast<const NativeFrame<Schema> *>(value)->valid();
    }

    const FrameStorage<Schema> &input_storage(const void *value) const noexcept override {
        return static_cast<const NativeFrame<Schema> *>(value)->scalar_storage();
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<NativeFrame<Schema> *>(destination));
    }
    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto *frame = static_cast<NativeFrame<Schema> *>(value);
        frame->release(memory);
        std::destroy_at(frame);
    }
    void invalidate_output(const OutputState &output) const override {
        auto *frame = static_cast<NativeFrame<Schema> *>(output.mutable_data());
        frame->invalidate(output.context().evaluation_time(), output.allocator());
        this->invalidate_output_items(output);
        this->invalidate_output_value(output);
    }

  private:
    template <std::size_t... Index>
    void append_item_layouts(const NativeFrame<Schema> &sample, std::index_sequence<Index...>) {
        const auto states = util::aggregate_members(sample.child_states());
        const auto values = util::aggregate_members(sample.scalar_storage().columns);
        const auto *base = reinterpret_cast<const std::byte *>(&sample);
        const auto append = [&]<std::size_t Field>() {
            using Storage = std::remove_cvref_t<decltype(std::get<Field>(values))>;
            using Cell = typename frame_column_storage<Storage>::cell_type;
            const auto &type = column_time_series_type<Cell>(TimeSeriesGuarantee::ordinary);
            const auto &runtime = column_view_time_series_runtime_type<Cell>(type);
            const auto *state = reinterpret_cast<const std::byte *>(std::addressof(std::get<Field>(states)));
            const auto *value = reinterpret_cast<const std::byte *>(std::addressof(std::get<Field>(values)));
            fields_.emplace_back(runtime, static_cast<std::size_t>(state - base),
                                 static_cast<std::size_t>(value - base));
        };
        (append.template operator()<Index>(), ...);
    }

    std::vector<ItemLayout> fields_;
};

template <template <template <typename, auto...> typename> typename Schema>
class FrameViewSeriesRuntimeType final : public FrameSeriesRuntimeType<Schema> {
  public:
    using Storage = FrameViewOutputStorage<Schema>;
    using ItemLayout = TimeSeriesRuntimeType::ItemLayout;
    using ConstItem = TimeSeriesRuntimeType::ConstItem;
    using MutableItem = TimeSeriesRuntimeType::MutableItem;
    using ItemIdFunction = TimeSeriesRuntimeType::ItemIdFunction;
    using ModifiedItemFunction = TimeSeriesRuntimeType::ModifiedItemFunction;

    explicit FrameViewSeriesRuntimeType(const TimeSeriesType &type)
        : FrameSeriesRuntimeType<Schema>{type, sizeof(Storage), alignof(Storage), true, true, true} {
        const auto sample = Storage{};
        append_item_layouts(sample, std::make_index_sequence<util::aggregate_member_count<Schema<ColumnStorage>>>{});
    }

    ItemLayout item_layout(item_id_t id) const override { return fields_.at(id); }
    std::size_t item_count(const void *) const override { return fields_.size(); }
    ConstItem item(const void *value, item_id_t id) const override {
        const auto &field = fields_.at(id);
        const auto *bytes = static_cast<const std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<const TimeSeriesState *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }
    MutableItem item(void *value, item_id_t id) const override {
        const auto &field = fields_.at(id);
        auto *bytes = static_cast<std::byte *>(value);
        return {field.runtime_type, *reinterpret_cast<TimeSeriesState *>(bytes + field.last_updated_offset),
                bytes + field.value_offset};
    }
    void for_each_item_id(const void *, void *context, ItemIdFunction function) const override {
        for (auto id = item_id_t{}; id < fields_.size(); ++id)
            function(context, id);
    }
    bool item_updated(const TimeSeriesState &last_updated, const void *value, item_id_t id) const noexcept override {
        const auto &storage = *static_cast<const Storage *>(value);
        return last_updated.valid() && storage.scalar.change_time == last_updated.time() &&
               storage.changed_columns.contains(id);
    }
    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &storage = *static_cast<const Storage *>(value);
        if (!last_updated.valid() || storage.scalar.change_time != last_updated.time())
            return;
        for (auto id : storage.changed_columns)
            function(context, id);
    }
    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const Storage *>(value);
        const auto *bytes = static_cast<const std::byte *>(value);
        const auto publish = [&](item_id_t id) {
            const auto &field = fields_[id];
            function(context, {id, &field.runtime_type,
                               reinterpret_cast<const TimeSeriesState *>(bytes + field.last_updated_offset),
                               bytes + field.value_offset});
        };
        if (!last_updated.valid()) {
            for (auto id = item_id_t{}; id < fields_.size(); ++id)
                publish(id);
            return;
        }
        if (storage.scalar.change_time != evaluation_time)
            return;
        for (auto id : storage.changed_columns)
            publish(id);
    }
    bool collection_valid(const void *value) const noexcept override {
        return static_cast<const Storage *>(value)->valid;
    }
    const FrameStorage<Schema> &input_storage(const void *value) const noexcept override {
        return static_cast<const Storage *>(value)->scalar;
    }
    void construct_initial_value(void *destination) const noexcept override { std::construct_at(static_cast<Storage *>(destination)); }
    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto &storage = *static_cast<Storage *>(value);
        storage.release(memory);
        std::destroy_at(&storage);
    }
    void invalidate_output(const OutputState &output) const override {
        auto &storage = *static_cast<Storage *>(output.mutable_data());
        storage.invalidate();
        this->invalidate_output_items(output);
        this->invalidate_output_value(output);
    }

  private:
    template <std::size_t... Index>
    void append_item_layouts(const Storage &sample, std::index_sequence<Index...>) {
        const auto states = util::aggregate_members(sample.child_states);
        const auto values = util::aggregate_members(sample.scalar.columns);
        const auto *base = reinterpret_cast<const std::byte *>(&sample);
        const auto append = [&]<std::size_t Field>() {
            using ColumnStorageType = std::remove_cvref_t<decltype(std::get<Field>(values))>;
            using Cell = typename frame_column_storage<ColumnStorageType>::cell_type;
            const auto &type = column_time_series_type<Cell>(TimeSeriesGuarantee::ordinary);
            const auto &runtime = column_view_time_series_runtime_type<Cell>(type);
            const auto *state = reinterpret_cast<const std::byte *>(std::addressof(std::get<Field>(states)));
            const auto *value = reinterpret_cast<const std::byte *>(std::addressof(std::get<Field>(values)));
            fields_.emplace_back(runtime, static_cast<std::size_t>(state - base),
                                 static_cast<std::size_t>(value - base));
        };
        (append.template operator()<Index>(), ...);
    }

    std::vector<ItemLayout> fields_;
};

} // namespace details

template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesType &frame_time_series_type(TimeSeriesGuarantee guarantee) {
    if (guarantee != TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"TSF requires ordinary"};
    const auto &type = frame_series_type(frame_schema_type<Schema>(), guarantee);
    if constexpr (!bg::details::schema_variable_template<Schema>) {
        const auto &runtime = details::canonical_frame_series_runtime_type(
            typeid(TSF<Schema>), type,
            [](const TimeSeriesType &series_type) -> std::unique_ptr<TimeSeriesRuntimeType> {
                return std::make_unique<details::NativeFrameSeriesRuntimeType<Schema>>(series_type);
            });
        register_time_series_runtime_type(runtime);
    }
    return type;
}

template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesRuntimeType &frame_time_series_runtime_type(const TimeSeriesType &type) {
    if constexpr (bg::details::schema_variable_template<Schema>) {
        throw std::logic_error{"runtime-schema TSF storage is not implemented"};
    } else {
        const auto &canonical = frame_time_series_type<Schema>(TimeSeriesGuarantee::ordinary);
        if (&type != &canonical)
            throw std::invalid_argument{"TSF runtime requires resolved ordinary type"};
        return details::canonical_frame_series_runtime_type(
            typeid(TSF<Schema>), canonical,
            [](const TimeSeriesType &series_type) -> std::unique_ptr<TimeSeriesRuntimeType> {
                return std::make_unique<details::NativeFrameSeriesRuntimeType<Schema>>(series_type);
            });
    }
}

template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesRuntimeType &frame_view_time_series_runtime_type(const TimeSeriesType &type) {
    if constexpr (bg::details::schema_variable_template<Schema>) {
        throw std::logic_error{"runtime-schema TSF views are not implemented"};
    } else {
        const auto &canonical = frame_time_series_type<Schema>(TimeSeriesGuarantee::ordinary);
        if (&type != &canonical)
            throw std::invalid_argument{"TSF view runtime requires resolved ordinary type"};
        static const auto runtime = details::FrameViewSeriesRuntimeType<Schema>{canonical};
        return runtime;
    }
}

} // namespace bg::frame

namespace bg {

template <template <template <typename, auto...> typename> typename Schema, auto... SeriesProperties,
          auto... Properties>
class In<frame::TSF<Schema, SeriesProperties...>, Properties...>
    : public InputActivityControl<InputProperties<Properties...>::dynamic_activity> {
    using ParsedProperties = InputProperties<Properties...>;
    using ActivityControl = InputActivityControl<ParsedProperties::dynamic_activity>;

  public:
    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : ActivityControl{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    std::size_t size() const noexcept { return valid() ? storage().size : 0; }
    bool contains(frame::row_id_t id) const noexcept { return valid() && storage().rows.has(id); }
    template <std::size_t Index> auto cell(frame::row_id_t id) const {
        using Cell = frame::details::frame_schema_cell_t<Index, Schema>;
        const auto fields = util::aggregate_members(storage().columns);
        return frame::ColumnSeriesView<Cell>{std::get<Index>(fields)}.cell(id);
    }
    const void *row_domain() const noexcept { return valid() ? storage().rows.identity : nullptr; }
    Scalar<frame::Frame<Schema>> value_view() const noexcept {
        return Scalar<frame::Frame<Schema>>::from(storage());
    }
    template <typename Function> void for_each_row(Function &&function) const {
        if (!valid())
            return;
        auto position = std::size_t{};
        storage().rows.each([&](frame::row_id_t id) { std::invoke(function, id, position++); });
    }
    template <typename Function> void for_each_changed(Function &&function) const {
        if (!changes_current() || storage().changed == nullptr)
            return;
        for (auto id : *storage().changed)
            std::invoke(function, id);
    }
    template <typename Function> void for_each_removed(Function &&function) const {
        if (!changes_current() || storage().removed == nullptr)
            return;
        for (auto id : *storage().removed)
            std::invoke(function, id);
    }
    template <std::size_t Index, typename Function> void for_each_modified(Function &&function) const {
        if (!changes_current() || storage().modified[Index] == nullptr)
            return;
        for (auto id : *storage().modified[Index])
            std::invoke(function, id);
    }
    frame::details::FrameInputDescriptor<Schema> extension_descriptor() const noexcept {
        return {std::addressof(storage()), valid(), modified(), last_updated()};
    }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }

    static constexpr auto name = ParsedProperties::name;
    static constexpr auto activity = ParsedProperties::activity;
    static constexpr auto validity = ParsedProperties::validity;
    static constexpr auto dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr auto dynamic_activity = ParsedProperties::dynamic_activity;

    using time_series_type = frame::TSF<Schema, SeriesProperties...>;

  private:
    const frame::FrameStorage<Schema> &storage() const noexcept {
        return static_cast<const frame::FrameSeriesRuntimeType<Schema> &>(data_.runtime_type())
            .input_storage(data_.data());
    }
    bool changes_current() const noexcept { return modified() && storage().change_time == last_updated(); }
    TimeSeriesInputData data_;
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
class Out<frame::TSF<Schema, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    frame::row_id_t add(const frame::Row<Schema> &row) const {
        const auto id = storage().add(row, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
        return id;
    }
    void set(frame::row_id_t id, const frame::RowPatch<Schema> &patch) const {
        if (storage().set(id, patch, binding_->context().evaluation_time(), binding_->allocator()))
            binding_->publish();
    }
    bool erase(frame::row_id_t id) const {
        const auto erased = storage().erase(id, binding_->context().evaluation_time(), binding_->allocator());
        if (erased)
            binding_->publish();
        return erased;
    }
    void clear() const {
        if (!binding_->valid() || storage().size() != 0) {
            storage().clear(binding_->context().evaluation_time(), binding_->allocator());
            binding_->publish();
        }
    }
    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    using time_series_type = frame::TSF<Schema, Properties...>;

  private:
    frame::NativeFrame<Schema> &storage() const noexcept {
        return *static_cast<frame::NativeFrame<Schema> *>(binding_->mutable_data());
    }
    TimeSeriesOutputBinding binding_;
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
class Out<frame::TSF<Schema, Properties...>, frame::view> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    template <template <template <typename, auto...> typename> typename SourceSchema,
              util::compile_time_string... Names>
    bool bind_projection(const In<frame::TSF<SourceSchema>> &source, frame::ColumnNames<Names...>) const {
        const auto input = source.extension_descriptor();
        auto &output = storage();
        if (!input.valid) {
            if (output.valid)
                binding_->invalidate();
            return false;
        }

        const auto changed = output.bind_projection(*input.storage, input.modified, input.last_updated,
                                                    binding_->context().evaluation_time(), binding_->allocator(),
                                                    frame::ColumnNames<Names...>{});
        if (changed) {
            for (auto index : output.changed_columns)
                binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
        return changed;
    }

    template <util::compile_time_string Name,
              template <template <typename, auto...> typename> typename SourceSchema,
              auto... SeriesProperties, auto... InputProperties, typename Predicate>
    bool bind_filter(const In<frame::TSF<SourceSchema, SeriesProperties...>, InputProperties...> &source,
                     Predicate &&predicate) const {
        const auto input = source.extension_descriptor();
        auto &output = storage();
        if (!input.valid) {
            if (output.valid)
                binding_->invalidate();
            return false;
        }

        const auto changed = output.template bind_filter<Name>(
            *input.storage, input.modified, input.last_updated, binding_->context().evaluation_time(),
            binding_->allocator(), std::forward<Predicate>(predicate));
        if (changed) {
            for (auto index : output.changed_columns)
                binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
        return changed;
    }

    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    using time_series_type = frame::TSF<Schema, Properties...>;

  private:
    frame::FrameViewOutputStorage<Schema> &storage() const noexcept {
        return *static_cast<frame::FrameViewOutputStorage<Schema> *>(binding_->mutable_data());
    }
    TimeSeriesOutputBinding binding_;
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct time_series_type_traits<frame::TSF<Schema, Properties...>> {
    using semantic_type = TimeSeriesType;
    static constexpr bool generic = details::schema_variable_template<Schema>;
    static constexpr bool supported = true;

    template <typename Scope, std::size_t Index, bool Input>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if (!guarantee.is_resolved()) {
            return frame::frame_series_type(frame::frame_schema_type<Schema>(), guarantee);
        }
        return frame::frame_time_series_type<Schema>(*guarantee.value());
    }
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_declaration_traits<frame::Frame<Schema>> {
    static constexpr bool generic = details::schema_variable_template<Schema>;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = true;
};

template <template <template <typename, auto...> typename> typename Schema>
struct declaration_variable_traits<frame::Frame<Schema>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        if constexpr (details::schema_variable_template<Schema>) {
            using Declaration = schema_type<Schema>;
            std::forward<Visitor>(visitor).template operator()<Declaration>(
                details::schema_variable_instance<Schema>());
        }
    }
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct declaration_variable_traits<frame::TSF<Schema, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        TimeSeriesDeclarationProperties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct time_series_runtime_traits<frame::TSF<Schema, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return frame::frame_time_series_runtime_type<Schema>(type);
    }
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct output_declaration_traits<Out<frame::TSF<Schema, Properties...>, frame::view>> {
    using time_series_type = frame::TSF<Schema, Properties...>;
    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return frame::frame_view_time_series_runtime_type<Schema>(type);
    }
};

} // namespace bg

namespace bg::frame {

namespace details {

template <template <template <typename, auto...> typename> typename SourceSchema,
          template <template <typename, auto...> typename> typename TargetSchema,
          util::compile_time_string... Names>
struct ProjectNode {
    void operator()(In<TSF<SourceSchema>> input, Out<TSF<TargetSchema>, frame::view> output) const {
        output.bind_projection(input, columns<Names...>());
    }
};

template <util::compile_time_string Name,
          template <template <typename, auto...> typename> typename Schema, typename Threshold>
struct FilterGreaterThanNode {
    static constexpr auto predicate_index = frame_column_index<Name, Schema>();
    static_assert(predicate_index < util::aggregate_member_count<Schema<Column>>,
                  "filter names an unknown predicate column");
    using Cell = frame_schema_cell_t<predicate_index, Schema>;
    static_assert(cell_type<Cell>, "filter predicate column must have a supported numeric cell type");
    static_assert(std::is_arithmetic_v<Threshold>, "filter threshold must be numeric");

    void operator()(In<TSF<Schema>, optional> input, Threshold threshold, Out<TSF<Schema>, frame::view> output) const {
        output.template bind_filter<Name>(input, [&](row_id_t id) {
            const auto value = input.template cell<predicate_index>(id);
            return value && *value > threshold;
        });
    }
};

} // namespace details

template <template <template <typename, auto...> typename> typename TargetSchema,
          util::compile_time_string... Names,
          template <template <typename, auto...> typename> typename SourceSchema, auto... Properties>
auto project(wp<TSF<SourceSchema, Properties...>> source) {
    static const auto projection =
        bg::node(details::ProjectNode<SourceSchema, TargetSchema, Names...>{});
    return projection(source);
}

template <util::compile_time_string Name,
          template <template <typename, auto...> typename> typename Schema, auto... Properties, typename Threshold>
    requires std::is_arithmetic_v<Threshold>
auto filter_greater_than(wp<TSF<Schema, Properties...>> source, Threshold threshold) {
    static const auto filter = bg::node(details::FilterGreaterThanNode<Name, Schema, Threshold>{});
    return filter(source, threshold);
}

} // namespace bg::frame

namespace bg {

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_type_traits<frame::Frame<Schema>> {
    static const ScalarType &type() {
        return frame::frame_type(frame::frame_schema_type<Schema>());
    }
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_storage_traits<frame::Frame<Schema>> {
    using stored_type = frame::FrameStorage<Schema>;
    using value_type = frame::FrameView<Schema>;
    static constexpr bool supports_byte_copy_initialization = true;
    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = true;
    static constexpr bool assign_uses_memory_resource = true;

    static value_type read(const stored_type &source) noexcept {
        constexpr auto member_count = util::aggregate_member_count<Schema<frame::ColumnStorage>>;
        return value_type{
            frame::details::frame_columns_view<Schema>(source.columns, std::make_index_sequence<member_count>{}),
            source.size};
    }

    static void construct(stored_type *destination, value_type source, allocator &memory) {
        std::construct_at(destination);
        try {
            copy_columns(*destination, source, memory);
            destination->size = source.size();
        } catch (...) {
            release_columns(*destination);
            std::destroy_at(destination);
            throw;
        }
    }

    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory) {
        construct(destination, read(source), memory);
    }

    static void assign(stored_type &destination, value_type source, allocator &memory) {
        auto replacement = stored_type{};
        auto release_replacement = util::scope_exit{[&]() noexcept { release_columns(replacement); }};
        copy_columns(replacement, source, memory);
        replacement.size = source.size();
        std::swap(destination, replacement);
    }

    static void destroy(stored_type *value, allocator &) noexcept {
        release_columns(*value);
        std::destroy_at(value);
    }

  private:
    template <std::size_t... Index>
    static void copy_columns(stored_type &destination, value_type source, allocator &memory,
                             std::index_sequence<Index...>) {
        auto destination_fields = util::aggregate_members(destination.columns);
        auto source_fields = util::aggregate_members(source.columns);
        (scalar_storage_traits<typename frame::details::frame_column_storage<
             std::remove_cvref_t<decltype(std::get<Index>(destination_fields))>>::scalar_type>::assign(
             std::get<Index>(destination_fields), std::get<Index>(source_fields), memory),
         ...);
    }

    static void copy_columns(stored_type &destination, value_type source, allocator &memory) {
        constexpr auto member_count = util::aggregate_member_count<Schema<frame::ColumnStorage>>;
        copy_columns(destination, source, memory, std::make_index_sequence<member_count>{});
    }

    template <std::size_t... Index>
    static void release_columns(stored_type &value, std::index_sequence<Index...>) noexcept {
        auto fields = util::aggregate_members(value.columns);
        (([&] {
             auto &field = std::get<Index>(fields);
             if (field.snapshot != nullptr) {
                 field.snapshot->release();
                 field.snapshot = nullptr;
             }
         }()),
         ...);
    }

    static void release_columns(stored_type &value) noexcept {
        constexpr auto member_count = util::aggregate_member_count<Schema<frame::ColumnStorage>>;
        release_columns(value, std::make_index_sequence<member_count>{});
    }
};

} // namespace bg
