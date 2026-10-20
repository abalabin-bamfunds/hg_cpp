#pragma once

#include <bg/frame/expression.hpp>
#include <bg/frame/literal.hpp>
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
#include <bg/type/schema.hpp>
#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/schema_metadata_values.hpp>
#include <bg/node/output_declaration_traits.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/util/compile_time_string.hpp>
#include <bg/util/contiguous_bunch.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/reflection.hpp>
#include <bg/util/static_storage.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <typeinfo>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace bg::frame {

// Erased key cells are used only by extension storage operations and retained removal deltas.
using KeyValue = std::variant<std::monostate, i64>;

template <util::compile_time_string... Names> bg::SchemaMetadata key_metadata() {
    static_assert(sizeof...(Names) != 0, "a frame key must name at least one column");
    auto names = std::array<std::string_view, sizeof...(Names)>{Names.view()...};
    auto value = bg::ScalarValue<bg::List<bg::string>>{names};
    return bg::make_schema_metadata({bg::schema_metadata_entry("bg.frame.key", value)});
}

template <typename T> class Cell {
  public:
    using value_type = std::conditional_t<bg::details::scalar_variable<T>, Scalar<>, T>;

    Cell() = delete;
    Cell(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    Cell(value_type value) : value_{std::move(value)} {}
    Cell(std::optional<value_type> value) : value_{std::move(value)} {}
    Cell(std::nullopt_t) noexcept {}
    const std::optional<value_type> &value() const noexcept { return value_; }

  private:
    std::optional<value_type> value_;
};

template <typename T> class PatchCell {
  public:
    using value_type = std::conditional_t<bg::details::scalar_variable<T>, Scalar<>, T>;

    PatchCell() noexcept = default;
    PatchCell(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    PatchCell(value_type value) : value_{std::move(value)}, changed_{true} {}
    PatchCell(std::optional<value_type> value) : value_{std::move(value)}, changed_{true} {}
    PatchCell(std::nullopt_t) noexcept : changed_{true} {}

    bool changed() const noexcept { return changed_; }
    const std::optional<value_type> &value() const noexcept { return value_; }

  private:
    std::optional<value_type> value_;
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
    bool supports_equality() const noexcept override { return true; }
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
    const TimeSeriesType &with_guarantee_limit(TimeSeriesGuarantee limit) const override;
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
template <template <template <typename, auto...> typename> typename Schema> class NativeFrame;
class NativeFrameStorage;
struct FrameColumnReadBinding;
namespace details {
struct NativeFrameLayout;
struct FrameSnapshotOwner;
}

struct FrameSnapshotStorage {
    details::FrameSnapshotOwner *owner{};
};

namespace details {
using frame_series_runtime_factory = std::unique_ptr<TimeSeriesRuntimeType> (*)(const TimeSeriesType &);
const TimeSeriesRuntimeType &canonical_frame_series_runtime_type(const TimeSeriesType &type,
                                                                 frame_series_runtime_factory factory);

inline constexpr auto null_key_hash = std::size_t{0x9e3779b9};

struct NativeFrameFieldLayout {
    using add_function = void (*)(void *, NativeRows &, row_id_t, const void *, allocator);
    using set_function = void (*)(void *, NativeRows &, row_id_t, const void *, allocator);
    using erase_function = void (*)(void *, row_id_t, row_id_t, allocator);
    using reset_function = void (*)(void *);
    using release_function = void (*)(void *, allocator);
    using modified_function = const util::index<> *(*)(const void *) noexcept;
    using bind_storage_function = void (*)(void *, const void *, const NativeRows &) noexcept;
    using copy_column_storage_function = void (*)(void *, const void *) noexcept;
    using construct_function = void (*)(void *) noexcept;
    using destroy_function = void (*)(void *) noexcept;
    using bind_column_view_function = void (*)(const void *, void *);
    using bind_column_series_view_function = void (*)(const void *, void *);
    using bind_view_storage_function = void (*)(void *, const FrameColumnReadBinding &, RowSet,
                                                const util::index<> *, const util::index<> *,
                                                const util::index<> *, EvaluationTime);
    using set_slots_function = bool (*)(void *, const NativeRowSlots &, const util::index<> &, row_id_t,
                                        const void *, allocator);
    using bind_computed_storage_function = void (*)(void *, const void *, const NativeRowSlots &, RowSet,
                                                    const util::index<> *, const util::index<> *,
                                                    const util::index<> *, EvaluationTime);
    using read_binding_function = FrameColumnReadBinding (*)(const void *);
    using copy_snapshot_column_function = void *(*)(const FrameColumnReadBinding &, std::size_t, allocator);
    using release_snapshot_column_function = void (*)(void *);
    using release_snapshot_storage_function = void (*)(void *, allocator);
    using install_snapshot_column_function = void (*)(void *, void *);
    using hash_key_value_function = std::size_t (*)(const void *) noexcept;
    using hash_row_key_function = std::size_t (*)(const void *, const NativeRows &, row_id_t) noexcept;
    using equal_row_key_function = bool (*)(const void *, const NativeRows &, row_id_t, const void *) noexcept;
    using append_row_key_function = void (*)(const void *, const NativeRows &, row_id_t,
                                              std::pmr::vector<KeyValue> &);
    using hash_erased_key_function = std::size_t (*)(const KeyValue &);
    using equal_erased_row_key_function = bool (*)(const void *, const NativeRows &, row_id_t,
                                                   const KeyValue &);

    const ScalarType *cell_type{};
    const TimeSeriesRuntimeType *column_runtime{};
    std::size_t native_offset{};
    std::size_t child_state_offset{};
    std::size_t column_storage_offset{};
    std::size_t native_size{};
    std::size_t native_alignment{};
    std::size_t child_state_size{};
    std::size_t child_state_alignment{};
    std::size_t column_storage_size{};
    std::size_t column_storage_alignment{};
    add_function add{};
    set_function set{};
    erase_function erase{};
    reset_function reset{};
    release_function release{};
    modified_function modified{};
    bind_storage_function bind_storage{};
    copy_column_storage_function copy_column_storage{};
    construct_function construct{};
    destroy_function destroy{};
    construct_function construct_storage{};
    destroy_function destroy_storage{};
    bind_column_view_function bind_column_view{};
    bind_column_series_view_function bind_column_series_view{};
    bind_view_storage_function bind_view_storage{};
    set_slots_function set_slots{};
    bind_computed_storage_function bind_computed_storage{};
    read_binding_function read_binding{};
    copy_snapshot_column_function copy_snapshot_column{};
    release_snapshot_column_function release_snapshot_column{};
    release_snapshot_storage_function release_snapshot_storage{};
    install_snapshot_column_function install_snapshot_column{};
    hash_key_value_function hash_key_value{};
    hash_row_key_function hash_row_key{};
    equal_row_key_function equal_row_key{};
    append_row_key_function append_row_key{};
    hash_erased_key_function hash_erased_key{};
    equal_erased_row_key_function equal_erased_row_key{};
};

struct NativeFrameCellPatch {
    bool changed{};
    const void *value{};
};

struct NativeFrameLayout {
    const bg::Schema *schema{};
    std::size_t size{};
    std::size_t alignment{};
    std::vector<NativeFrameFieldLayout> fields;
    std::vector<std::size_t> key_fields;
    std::size_t key_state_offset{std::numeric_limits<std::size_t>::max()};
};

struct NativeFrameKeyState {
    using KeyTuple = std::pmr::vector<KeyValue>;

    util::hash_dictionary<row_id_t, row_id_t> live_rows;
    util::hash_dictionary<row_id_t, KeyTuple> removed_rows;

    void release(allocator memory) noexcept {
        live_rows.release(memory.memory_resource());
        removed_rows.release_with(memory.memory_resource(), [](row_id_t *) noexcept {}, [](KeyTuple *key) noexcept {
            key->~KeyTuple();
        });
    }
};

const NativeFrameLayout &native_frame_layout(const SchemaType &schema);
NativeFrameFieldLayout native_frame_field_layout(const ScalarType &cell_type);

template <template <template <typename, auto...> typename> typename Schema>
const NativeFrameLayout &native_frame_layout() {
    return native_frame_layout(frame_schema_type<Schema>());
}
}

template <template <template <typename, auto...> typename> typename Schema> struct FrameStorage {
    static constexpr bool supports_byte_copy_initialization = true;
    static constexpr auto field_count =
        bg::details::hierarchical_scalar_schema_reflection<Schema>::field_count;
    Schema<ColumnStorage> columns;
    RowSet rows{};
    const util::index<> *added{};
    const util::index<> *removed{};
    const util::index<> *changed{};
    std::array<const util::index<> *, field_count> modified{};
    const util::index<> *changed_columns{};
    EvaluationTime change_time{MIN_DT};
    bool valid{};
    std::size_t size{};
};

// Extension-facing borrowed binding for one resolved frame column. Its payload follows the source owner.
struct FrameColumnReadBinding {
    const ScalarType *cell_type{};
    const void *payload{};
    void (*bind_column_view)(const void *, void *){};
    void (*bind_column_series_view)(const void *, void *){};
    void (*copy_column_storage)(void *, const void *) noexcept{};
    const void *native{};
    const NativeRows *rows{};
    const void *snapshot{};
    RowSet visible_rows{};
    RowSet coverage{};
    const NativeRowSlots *physical_rows{};
};

// Extension-facing borrowed descriptor. Copies retain no owner; the source owner controls its lifetime. Descriptors
// obtained from node inputs are valid only for that invocation.
struct FrameReadStorage {
    using changed_column_visitor = void (*)(void *, std::size_t);
    using column_binding_function = FrameColumnReadBinding (*)(const void *, std::size_t);

    const bg::Schema *schema{};
    RowSet rows{};
    bool valid{};
    bool modified{};
    std::size_t size{};
    const util::index<> *added{};
    const util::index<> *removed{};
    const util::index<> *changed{};
    std::span<const util::index<> *const> column_modified;
    const void *column_modified_context{};
    const util::index<> *(*modified_column)(const void *, std::size_t) noexcept{};
    EvaluationTime change_time{MIN_DT};
    EvaluationTime last_updated{MIN_DT};
    const void *columns_context{};
    column_binding_function column_binding{};
    const void *changed_columns_context{};
    bool (*column_changed)(const void *, std::size_t) noexcept{};
    void (*for_each_changed_column)(const void *, void *, changed_column_visitor){};
    const void *source_owner{};
    const RuntimeType *source_runtime{};

    std::size_t field_index(util::symbol name) const;
    template <template <template <typename, auto...> typename> typename Schema>
    auto field_ordinals() const;
    const util::index<> *modified_at(util::symbol name) const { return modified_at(field_index(name)); }
    FrameColumnReadBinding column(std::size_t index) const;
    ColumnView<> generic_column(std::size_t index) const {
        const auto binding = column(index);
        if (binding.cell_type == nullptr || binding.bind_column_view == nullptr)
            throw std::logic_error{"frame column has no readable cell type"};
        if (binding.cell_type == std::addressof(cpp_scalar_type<i64>())) {
            auto result = ColumnView<i64>{};
            binding.bind_column_view(binding.payload, std::addressof(result));
            return ColumnView<>{result};
        }
        if (binding.cell_type == std::addressof(cpp_scalar_type<double>())) {
            auto result = ColumnView<double>{};
            binding.bind_column_view(binding.payload, std::addressof(result));
            return ColumnView<>{result};
        }
        throw std::invalid_argument{"frame columns support only i64 and double cell types"};
    }
    const util::index<> *modified_at(std::size_t index) const noexcept {
        if (modified_column != nullptr)
            return modified_column(column_modified_context, index);
        return index < column_modified.size() ? column_modified[index] : nullptr;
    }

    template <cell_type T> void bind_column_view(util::symbol name, ColumnView<T> &destination) const;
    template <cell_type T>
    void bind_column_series_view(util::symbol name,
                                 std::optional<ColumnSeriesView<T>> &destination) const;
    template <template <template <typename, auto...> typename> typename Schema>
    FrameStorage<Schema> typed_storage() const;
    template <template <template <typename, auto...> typename> typename Schema, std::size_t Count>
    FrameStorage<Schema> typed_storage(const std::array<std::size_t, Count> &ordinals) const;
    bool has_changed_column(std::size_t index) const noexcept {
        return changed_columns_context != nullptr && column_changed != nullptr &&
               column_changed(changed_columns_context, index);
    }
    void each_changed_column(void *context, changed_column_visitor visit) const {
        if (changed_columns_context != nullptr && for_each_changed_column != nullptr)
            for_each_changed_column(changed_columns_context, context, visit);
    }

};

namespace details {
void validate_frame_key_uniqueness(const FrameReadStorage &source);
}

// Common scalar runtime interface for native, Arrow, and snapshot frame representations.
class FrameScalarRuntimeType : public ScalarRuntimeType {
  public:
    virtual FrameReadStorage read_storage(const ScalarData &source) const = 0;
    virtual details::FrameSnapshotOwner *snapshot_owner(const ScalarData &) const noexcept { return nullptr; }
    const ScalarRuntimeType &runtime_type_for_copy(const ScalarData &source) const override;
    bool equals(const void *value, const ScalarData &other) const override;
    bool equals(ScalarDataMode mode, std::uintptr_t data, const ScalarData &other) const override;
    bool equals_same_type(const void *value, const ScalarData &other) const override;

  protected:
    using ScalarRuntimeType::ScalarRuntimeType;
};

class FrameSeriesRuntimeType;
const ScalarRuntimeType &frame_series_scalar_runtime_type(const FrameSeriesRuntimeType &runtime);
const ScalarRuntimeType &frame_snapshot_runtime_type(const FrameType &type);

class FrameSeriesRuntimeType : public TimeSeriesRuntimeType {
  public:
    virtual FrameReadStorage input_storage(const void *value) const noexcept = 0;
    const ScalarRuntimeType &scalar_runtime_type() const {
        if (const auto *runtime = scalar_runtime_.load(std::memory_order_acquire))
            return *runtime;
        return frame_series_scalar_runtime_type(*this);
    }
    const ScalarRuntimeType *cached_scalar_runtime_type() const noexcept {
        return scalar_runtime_.load(std::memory_order_acquire);
    }
    void cache_scalar_runtime_type(const ScalarRuntimeType &runtime) const noexcept {
        auto *empty = static_cast<const ScalarRuntimeType *>(nullptr);
        if (!scalar_runtime_.compare_exchange_strong(empty, std::addressof(runtime), std::memory_order_release,
                                                    std::memory_order_relaxed) && empty != std::addressof(runtime))
            std::terminate();
    }

  protected:
    FrameSeriesRuntimeType(const TimeSeriesType &type, std::size_t size, std::size_t alignment,
                           bool uses_variable_memory, bool requires_finalization,
                           bool supports_byte_copy_initialization) noexcept
        : TimeSeriesRuntimeType{type, size, alignment, uses_variable_memory, requires_finalization,
                                supports_byte_copy_initialization} {}

  private:
    mutable std::atomic<const ScalarRuntimeType *> scalar_runtime_{};
};

namespace details {

template <template <template <typename, auto...> typename> typename Schema>
using frame_schema_reflection = bg::details::hierarchical_scalar_schema_reflection<Schema>;

template <typename T, bool Generic = bg::details::scalar_variable<T>> struct FrameViewColumnType {
    using type = ColumnView<T>;
};
template <typename T> struct FrameViewColumnType<T, true> {
    using type = ColumnView<>;
};
template <typename T, auto... Parameters>
using FrameViewColumn = typename FrameViewColumnType<T>::type;

template <template <template <typename, auto...> typename> typename Schema,
          bool RuntimeSchema = bg::details::schema_variable_template<Schema>>
struct FrameViewColumnsType {
    using type = Schema<FrameViewColumn>;
};
template <template <template <typename, auto...> typename> typename Schema>
struct FrameViewColumnsType<Schema, true> {
    using type = Schema<ColumnView>;
};

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema>
using frame_schema_cell_t = typename frame_schema_reflection<Schema>::template field<Index>::scalar_type;

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
consteval bool frame_schema_fields_generic(std::index_sequence<Index...>) {
    return (false || ... || bg::details::scalar_variable<frame_schema_cell_t<Index, Schema>>);
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
consteval bool frame_schema_fields_supported(std::index_sequence<Index...>) {
    return (true && ... && (cell_type<frame_schema_cell_t<Index, Schema>> ||
                            bg::details::scalar_variable<frame_schema_cell_t<Index, Schema>>));
}

template <template <template <typename, auto...> typename> typename Schema, typename Visitor, std::size_t... Index>
void for_each_frame_schema_field_variable(Visitor &&visitor, std::index_sequence<Index...>) {
    (bg::for_each_declaration_variable<frame_schema_cell_t<Index, Schema>>(visitor), ...);
}

template <template <template <typename, auto...> typename> typename Schema>
struct frame_schema_declaration_traits {
    using Reflection = frame_schema_reflection<Schema>;

    static constexpr auto is_schema_variable = bg::details::schema_variable_template<Schema>;
    static constexpr auto has_generic_fields = [] {
        if constexpr (is_schema_variable)
            return false;
        else
            return frame_schema_fields_generic<Schema>(std::make_index_sequence<Reflection::field_count>{});
    }();
    static constexpr auto fields_supported = [] {
        if constexpr (is_schema_variable)
            return true;
        else
            return frame_schema_fields_supported<Schema>(std::make_index_sequence<Reflection::field_count>{});
    }();

    template <typename Visitor> static void for_each(Visitor &&visitor) {
        if constexpr (is_schema_variable) {
            using Declaration = bg::schema_type<Schema>;
            visitor.template operator()<Declaration>(bg::details::schema_variable_instance<Schema>());
        } else {
            for_each_frame_schema_field_variable<Schema>(visitor,
                                                         std::make_index_sequence<Reflection::field_count>{});
        }
    }
};

template <template <template <typename, auto...> typename> typename Schema>
consteval auto frame_schema_field_names() {
    using Reflection = frame_schema_reflection<Schema>;
    return []<std::size_t... Index>(std::index_sequence<Index...>) {
        return std::array<std::string_view, sizeof...(Index)>{Reflection::template field_name<Index>()...};
    }(std::make_index_sequence<Reflection::field_count>{});
}

template <typename T> struct frame_column_storage {
    using scalar_type = void;
};

template <std::size_t Index, util::compile_time_string... Names>
consteval auto frame_column_name(ColumnNames<Names...>) {
    constexpr auto names = std::array{Names...};
    return names[Index];
}

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema>
const util::symbol &frame_declared_field_symbol() {
    static const auto &schema = static_cast<const bg::Schema &>(frame_schema_type<Schema>());
    static const auto name = schema.field(Index).name;
    return name;
}
template <typename T> struct frame_column_storage<ColumnStorage<T>> {
    using scalar_type = Column<T>;
    using cell_type = T;
};

bool frame_column_changed(const void *context, std::size_t index) noexcept;
void frame_for_each_changed_column(const void *context, void *target,
                                   FrameReadStorage::changed_column_visitor visit);

template <cell_type Cell>
void bind_frame_column_view(const void *payload, void *destination) {
    *static_cast<ColumnView<Cell> *>(destination) =
        scalar_storage_traits<Column<Cell>>::read(*static_cast<const ColumnStorage<Cell> *>(payload));
}

template <cell_type Cell>
void bind_frame_column_series_view(const void *payload, void *destination) {
    static_cast<std::optional<ColumnSeriesView<Cell>> *>(destination)->emplace(
        *static_cast<const ColumnStorage<Cell> *>(payload));
}

template <cell_type Cell> void copy_frame_column_storage(void *destination, const void *source) noexcept {
    *static_cast<ColumnStorage<Cell> *>(destination) = *static_cast<const ColumnStorage<Cell> *>(source);
}

template <cell_type Cell> FrameColumnReadBinding frame_column_read_binding(const void *payload) {
    const auto &column = *static_cast<const ColumnStorage<Cell> *>(payload);
    return {std::addressof(cpp_scalar_type<Cell>()), payload, &bind_frame_column_view<Cell>,
            &bind_frame_column_series_view<Cell>, &copy_frame_column_storage<Cell>, column.native,
            column.rows, column.snapshot, column.visible_rows, column.coverage, column.physical_rows};
}

template <cell_type Cell>
void *copy_frame_snapshot_column(const FrameColumnReadBinding &source, std::size_t size, allocator memory) {
    if (source.cell_type != std::addressof(cpp_scalar_type<Cell>()) || source.bind_column_view == nullptr)
        throw std::invalid_argument{"frame snapshot column type does not match its schema"};
    auto column = ColumnView<Cell>{};
    source.bind_column_view(source.payload, std::addressof(column));
    auto *snapshot = ColumnSnapshot<Cell>::create(memory, size);
    try {
        auto position = std::size_t{};
        column.batches([&](const auto &batch) {
            if (batch.size > size - position)
                throw std::invalid_argument{"frame snapshot column has more rows than its schema"};
            for (auto index = std::size_t{}; index != batch.size; ++index, ++position) {
                if (!batch.is_valid(index))
                    continue;
                snapshot->value_at(position) = batch.values[index];
                snapshot->set_valid(static_cast<row_id_t>(position));
            }
        });
        if (position != size)
            throw std::invalid_argument{"frame snapshot column has fewer rows than its schema"};
    } catch (...) {
        snapshot->release();
        throw;
    }
    return snapshot;
}

template <cell_type Cell> void release_frame_snapshot_column(void *snapshot) {
    static_cast<ColumnSnapshot<Cell> *>(snapshot)->release();
}

template <cell_type Cell>
void release_frame_snapshot_storage(void *storage, allocator) {
    auto &column = *static_cast<ColumnStorage<Cell> *>(storage);
    if (column.snapshot != nullptr) {
        column.snapshot->release();
        column.snapshot = nullptr;
    }
}

template <cell_type Cell> void install_frame_snapshot_column(void *storage, void *snapshot) {
    *static_cast<ColumnStorage<Cell> *>(storage) =
        ColumnStorage<Cell>{static_cast<ColumnSnapshot<Cell> *>(snapshot)};
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t Index>
FrameColumnReadBinding frame_column_binding(const void *context) {
    using Cell = frame_schema_cell_t<Index, Schema>;
    const auto fields = frame_schema_reflection<Schema>::fields(*static_cast<const Schema<ColumnStorage> *>(context));
    return frame_column_read_binding<Cell>(std::addressof(std::get<Index>(fields)));
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
FrameColumnReadBinding frame_column_binding(const void *context, std::size_t index,
                                            std::index_sequence<Index...>) {
    using binding_function = FrameColumnReadBinding (*)(const void *);
    static constexpr std::array<binding_function, sizeof...(Index)> bindings{
        &frame_column_binding<Schema, Index>...};
    return index < bindings.size() ? bindings[index](context) : FrameColumnReadBinding{};
}

template <template <template <typename, auto...> typename> typename Schema>
FrameColumnReadBinding frame_column_binding(const void *context, std::size_t index) {
    return frame_column_binding<Schema>(
        context, index, std::make_index_sequence<frame_schema_reflection<Schema>::field_count>{});
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t Index>
void bind_typed_frame_column(FrameStorage<Schema> &storage, const FrameReadStorage &source,
                             std::size_t source_index) {
    using Cell = frame_schema_cell_t<Index, Schema>;
    const auto binding = source.column(source_index);
    if (binding.cell_type != std::addressof(cpp_scalar_type<Cell>()) || binding.payload == nullptr ||
        binding.copy_column_storage == nullptr)
        throw std::invalid_argument{"frame column type does not match the requested schema"};
    auto target_fields = frame_schema_reflection<Schema>::fields(storage.columns);
    binding.copy_column_storage(std::addressof(std::get<Index>(target_fields)), binding.payload);
    storage.modified[Index] = source.modified_at(source_index);
}

inline bool frame_column_changed(const void *context, std::size_t index) noexcept {
    return static_cast<const util::index<> *>(context)->contains(index);
}

inline void frame_for_each_changed_column(const void *context, void *target,
                                          FrameReadStorage::changed_column_visitor visit) {
    for (const auto index : *static_cast<const util::index<> *>(context))
        visit(target, index);
}

template <template <template <typename, auto...> typename> typename Schema, typename Storage, std::size_t... Index>
auto frame_columns_view(const Storage &storage, std::index_sequence<Index...>) {
    auto result = Schema<ColumnView>{};
    auto source = frame_schema_reflection<Schema>::fields(storage);
    auto destination = frame_schema_reflection<Schema>::fields(result);
    ((std::get<Index>(destination) = scalar_storage_traits<typename frame_column_storage<
          std::remove_cvref_t<decltype(std::get<Index>(source))>>::scalar_type>::read(std::get<Index>(source))),
     ...);
    return result;
}

template <template <template <typename, auto...> typename> typename Schema>
FrameReadStorage frame_read_storage(const FrameStorage<Schema> &storage, const void *owner,
    const RuntimeType *runtime, bool scalar_value = false) {
    static_assert(!bg::details::schema_variable_template<Schema>);
    auto result = FrameReadStorage{};
    result.schema = std::addressof(bg::details::cpp_hierarchical_scalar_schema<Schema>());
    result.rows = storage.rows;
    result.valid = scalar_value || storage.valid;
    result.size = storage.size;
    result.added = storage.added;
    result.removed = storage.removed;
    result.changed = storage.changed;
    result.column_modified = {storage.modified.data(), storage.modified.size()};
    result.change_time = storage.change_time;
    result.columns_context = std::addressof(storage.columns);
    result.column_binding = &frame_column_binding<Schema>;
    result.changed_columns_context = storage.changed_columns;
    if (storage.changed_columns != nullptr) {
        result.column_changed = &frame_column_changed;
        result.for_each_changed_column = &frame_for_each_changed_column;
    }
    result.source_owner = owner == nullptr ? std::addressof(storage) : owner;
    result.source_runtime = runtime;
    return result;
}

template <util::compile_time_string Name, template <template <typename, auto...> typename> typename Schema>
consteval std::size_t frame_column_index() {
    constexpr auto names = frame_schema_field_names<Schema>();
    for (auto index = std::size_t{}; index != names.size(); ++index) {
        if (names[index] == Name.view())
            return index;
    }
    return names.size();
}

} // namespace details

inline std::size_t FrameReadStorage::field_index(util::symbol name) const {
    if (schema == nullptr)
        throw std::logic_error{"frame read storage has no schema"};
    const auto fields = schema->fields();
    for (auto index = std::size_t{}; index != fields.size(); ++index)
        if (fields[index].name == name)
            return index;
    throw std::invalid_argument{"frame column is absent from the retained schema"};
}

template <template <template <typename, auto...> typename> typename Schema>
auto FrameReadStorage::field_ordinals() const {
    constexpr auto count = details::frame_schema_reflection<Schema>::field_count;
    auto result = std::array<std::size_t, count>{};
    if (schema == std::addressof(bg::details::cpp_hierarchical_scalar_schema<Schema>())) {
        for (auto index = std::size_t{}; index != count; ++index)
            result[index] = index;
        return result;
    }
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((result[Index] = field_index(details::frame_declared_field_symbol<Index, Schema>())), ...);
    }(std::make_index_sequence<count>{});
    return result;
}

inline FrameColumnReadBinding FrameReadStorage::column(std::size_t index) const {
    if (schema == nullptr || index >= schema->field_count() || column_binding == nullptr)
        throw std::out_of_range{"frame column index"};
    return column_binding(columns_context, index);
}

template <template <template <typename, auto...> typename> typename Schema>
inline FrameStorage<Schema> FrameReadStorage::typed_storage() const {
    return typed_storage<Schema>(field_ordinals<Schema>());
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t Count>
inline FrameStorage<Schema> FrameReadStorage::typed_storage(const std::array<std::size_t, Count> &ordinals) const {
    static_assert(Count == details::frame_schema_reflection<Schema>::field_count);
    auto result = FrameStorage<Schema>{};
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        (details::bind_typed_frame_column<Schema, Index>(result, *this, ordinals[Index]), ...);
    }(std::make_index_sequence<Count>{});
    result.rows = rows;
    result.added = added;
    result.removed = removed;
    result.changed = changed;
    result.change_time = change_time;
    result.valid = valid;
    result.size = size;
    return result;
}

template <cell_type T> void FrameReadStorage::bind_column_view(util::symbol name, ColumnView<T> &destination) const {
    const auto binding = column(field_index(name));
    if (binding.cell_type != std::addressof(cpp_scalar_type<T>()) || binding.bind_column_view == nullptr)
        throw std::invalid_argument{"frame column type does not match the requested cell type"};
    binding.bind_column_view(binding.payload, std::addressof(destination));
}

template <cell_type T>
void FrameReadStorage::bind_column_series_view(util::symbol name,
                                               std::optional<ColumnSeriesView<T>> &destination) const {
    const auto binding = column(field_index(name));
    if (binding.cell_type != std::addressof(cpp_scalar_type<T>()) || binding.bind_column_series_view == nullptr)
        throw std::invalid_argument{"frame column type does not match the requested cell type"};
    binding.bind_column_series_view(binding.payload, std::addressof(destination));
}

template <template <template <typename, auto...> typename> typename Schema> class FrameView {
  public:
    using columns_type = typename details::FrameViewColumnsType<Schema>::type;

    FrameView() = default;
    FrameView(columns_type columns, std::size_t size, ScalarData source = {})
        : columns_{std::move(columns)}, size_{size}, source_{source} {}
    explicit FrameView(const Frame<Schema> &frame);

    std::size_t size() const noexcept { return size_; }
    const columns_type &columns() const noexcept { return columns_; }
    const ScalarData &source_data() const noexcept { return source_; }
    const bg::Schema &schema() const;
    ColumnView<> column(std::size_t index) const;
    ColumnView<> column(util::symbol name) const;
    ColumnView<> column(std::string_view name) const { return column(util::symbol{name}); }

  private:
    columns_type columns_;
    std::size_t size_{};
    ScalarData source_;
};

template <template <template <typename, auto...> typename> typename Schema> class Frame {
  public:
    using columns_type = Schema<Column>;

    Frame() {
        if constexpr (!bg::details::schema_variable_template<Schema>)
            static_cast<void>(frame_type(schema()));
    }
    explicit Frame(columns_type columns) : columns_{std::move(columns)}, size_{column_size(columns_)} {
        validate_column_sizes();
        bind_storage();
        validate_key_uniqueness();
    }
    Frame(columns_type columns, std::size_t size) : columns_{std::move(columns)}, size_{size} {
        validate_column_sizes();
        bind_storage();
        validate_key_uniqueness();
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
    const bg::Schema &schema() const { return bg::details::cpp_hierarchical_scalar_schema<Schema>(); }
    ColumnView<> column(std::size_t index) const;
    ColumnView<> column(util::symbol name) const;
    ColumnView<> column(std::string_view name) const { return column(util::symbol{name}); }

  private:
    static std::size_t column_size(const columns_type &columns) {
        if constexpr (details::frame_schema_reflection<Schema>::field_count == 0)
            return 0;
        else {
            const auto members = details::frame_schema_reflection<Schema>::fields(columns);
            return std::get<0>(members).size();
        }
    }
    void validate_column_sizes() const {
        if constexpr (details::frame_schema_reflection<Schema>::field_count != 0) {
            const auto members = details::frame_schema_reflection<Schema>::fields(columns_);
            std::apply(
                [&](const auto &first, const auto &...rest) {
                    if (first.size() != size_ || ((rest.size() != size_) || ...))
                        throw std::invalid_argument{"frame columns have different lengths"};
                },
                members);
        }
    }
    void validate_key_uniqueness() const;
    void bind_storage() {
        auto destination = details::frame_schema_reflection<Schema>::fields(storage_.columns);
        auto source = details::frame_schema_reflection<Schema>::fields(columns_);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((std::get<Index>(destination) = std::get<Index>(source).storage()), ...);
        }(std::make_index_sequence<details::frame_schema_reflection<Schema>::field_count>{});
        storage_.size = size_;
    }

    columns_type columns_;
    std::size_t size_{};
    FrameStorage<Schema> storage_;
};

template <template <template <typename, auto...> typename> typename Schema>
FrameView<Schema>::FrameView(const Frame<Schema> &frame) : size_{frame.size()} {
    auto source = details::frame_schema_reflection<Schema>::fields(frame.columns());
    auto destination = details::frame_schema_reflection<Schema>::fields(columns_);
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((std::get<Index>(destination) = std::get<Index>(source).view()), ...);
    }(std::make_index_sequence<details::frame_schema_reflection<Schema>::field_count>{});
}

namespace details {

template <template <template <typename, auto...> typename> typename Schema, std::size_t Index>
void bind_frame_view_column(const void *payload, void *destination) {
    using Cell = frame_schema_cell_t<Index, Schema>;
    const auto columns = frame_schema_reflection<Schema>::fields(
        *static_cast<const Schema<FrameViewColumn> *>(payload));
    if constexpr (bg::details::scalar_variable<Cell>) {
        *static_cast<ColumnView<Cell> *>(destination) = std::get<Index>(columns).template view<Cell>();
    } else {
        *static_cast<ColumnView<Cell> *>(destination) = std::get<Index>(columns);
    }
}

template <cell_type Cell> void bind_generic_frame_view_column(const void *payload, void *destination) {
    *static_cast<ColumnView<Cell> *>(destination) = static_cast<const ColumnView<> *>(payload)->template view<Cell>();
}

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
FrameColumnReadBinding frame_view_column_binding(const void *context, std::size_t index,
                                                 std::index_sequence<Index...>) {
    using binding_function = FrameColumnReadBinding (*)(const void *);
    static constexpr std::array<binding_function, sizeof...(Index)> bindings{
        +[](const void *payload) -> FrameColumnReadBinding {
            using Cell = frame_schema_cell_t<Index, Schema>;
            if constexpr (bg::details::scalar_variable<Cell>) {
                const auto columns = frame_schema_reflection<Schema>::fields(
                    *static_cast<const Schema<FrameViewColumn> *>(payload));
                const auto &column = std::get<Index>(columns);
                const auto &cell_type = column.cell_type();
                if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>()))
                    return {std::addressof(cell_type), std::addressof(column),
                            &bind_generic_frame_view_column<i64>, nullptr};
                if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<double>()))
                    return {std::addressof(cell_type), std::addressof(column),
                            &bind_generic_frame_view_column<double>, nullptr};
                throw std::invalid_argument{"frame columns support only i64 and double cell types"};
            } else {
                return {std::addressof(cpp_scalar_type<Cell>()), payload,
                        &bind_frame_view_column<Schema, Index>, nullptr};
            }
        }...};
    return index < bindings.size() ? bindings[index](context) : FrameColumnReadBinding{};
}

template <template <template <typename, auto...> typename> typename Schema>
FrameReadStorage frame_read_storage(const FrameView<Schema> &view, const RuntimeType *runtime) {
    constexpr auto count = frame_schema_reflection<Schema>::field_count;
    auto result = FrameReadStorage{};
    result.schema = std::addressof(bg::details::cpp_hierarchical_scalar_schema<Schema>());
    result.valid = true;
    result.size = view.size();
    result.columns_context = std::addressof(view.columns());
    result.column_binding = [](const void *context, std::size_t index) {
        return frame_view_column_binding<Schema>(context, index, std::make_index_sequence<count>{});
    };
    result.source_owner = std::addressof(view);
    result.source_runtime = runtime;
    return result;
}

} // namespace details

template <template <template <typename, auto...> typename> typename Schema>
void Frame<Schema>::validate_key_uniqueness() const {
    if constexpr (!bg::details::schema_variable_template<Schema>) {
        auto view = FrameView<Schema>{*this};
        details::validate_frame_key_uniqueness(details::frame_read_storage(view, nullptr));
    }
}

template <template <template <typename, auto...> typename> typename Schema>
const bg::Schema &FrameView<Schema>::schema() const {
    if (source_.empty()) {
        if constexpr (bg::details::schema_variable_template<Schema>)
            throw std::logic_error{"runtime-schema frame view has no retained schema"};
        else
            return bg::details::cpp_hierarchical_scalar_schema<Schema>();
    }
    const auto &runtime = static_cast<const FrameScalarRuntimeType &>(source_.runtime_type());
    const auto storage = runtime.read_storage(source_);
    if (storage.schema == nullptr)
        throw std::logic_error{"frame scalar view has no schema"};
    return *storage.schema;
}

template <template <template <typename, auto...> typename> typename Schema>
ColumnView<> FrameView<Schema>::column(std::size_t index) const {
    if (source_.empty())
        return details::frame_read_storage(*this, nullptr).generic_column(index);
    const auto &runtime = static_cast<const FrameScalarRuntimeType &>(source_.runtime_type());
    return runtime.read_storage(source_).generic_column(index);
}

template <template <template <typename, auto...> typename> typename Schema>
ColumnView<> FrameView<Schema>::column(util::symbol name) const {
    if (source_.empty())
        return column(details::frame_read_storage(*this, nullptr).field_index(name));
    const auto &runtime = static_cast<const FrameScalarRuntimeType &>(source_.runtime_type());
    const auto storage = runtime.read_storage(source_);
    return storage.generic_column(storage.field_index(name));
}

template <template <template <typename, auto...> typename> typename Schema>
ColumnView<> Frame<Schema>::column(std::size_t index) const {
    auto storage = details::frame_read_storage(scalar_storage(), this, nullptr, true);
    return storage.generic_column(index);
}

template <template <template <typename, auto...> typename> typename Schema>
ColumnView<> Frame<Schema>::column(util::symbol name) const {
    auto storage = details::frame_read_storage(scalar_storage(), this, nullptr, true);
    return storage.generic_column(storage.field_index(name));
}

namespace details {

template <template <template <typename, auto...> typename> typename Schema>
FrameView<Schema> frame_view(const FrameReadStorage &storage, const ScalarData &source) {
    auto columns = typename FrameView<Schema>::columns_type{};
    if constexpr (!bg::details::schema_variable_template<Schema>) {
        constexpr auto count = frame_schema_reflection<Schema>::field_count;
        const auto ordinals = storage.template field_ordinals<Schema>();
        auto destination = frame_schema_reflection<Schema>::fields(columns);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (([&] {
                 using Cell = frame_schema_cell_t<Index, Schema>;
                 const auto binding = storage.column(ordinals[Index]);
                 if constexpr (bg::details::scalar_variable<Cell>) {
                     if (storage.schema == nullptr || binding.cell_type == nullptr ||
                         binding.cell_type != storage.schema->field(ordinals[Index]).type)
                         throw std::invalid_argument{"frame column type does not match the requested schema"};
                     std::get<Index>(destination) = storage.generic_column(ordinals[Index]);
                 } else {
                     if (binding.cell_type != std::addressof(cpp_scalar_type<Cell>()) ||
                         binding.bind_column_view == nullptr)
                         throw std::invalid_argument{"frame column type does not match the requested schema"};
                     binding.bind_column_view(binding.payload, std::addressof(std::get<Index>(destination)));
                 }
             }()),
             ...);
        }(std::make_index_sequence<count>{});
    }
    return FrameView<Schema>{std::move(columns), storage.size, source};
}

template <template <template <typename, auto...> typename> typename Schema>
class FrameViewScalarRuntimeType final : public FrameScalarRuntimeType {
  public:
    FrameViewScalarRuntimeType()
        : FrameScalarRuntimeType{frame_type(frame_schema_type<Schema>()), sizeof(FrameView<Schema>),
                                 alignof(FrameView<Schema>), false, false, false} {}

    FrameReadStorage read_storage(const ScalarData &source) const override {
        const auto &view = *static_cast<const FrameView<Schema> *>(source.data());
        if (!view.source_data().empty()) {
            const auto &source_runtime = static_cast<const FrameScalarRuntimeType &>(view.source_data().runtime_type());
            return source_runtime.read_storage(view.source_data());
        }
        return frame_read_storage(view, this);
    }
    void read_value(ScalarDataMode, std::uintptr_t, void *) const override { std::terminate(); }
    void construct_initial_value(void *) const noexcept override { std::terminate(); }
    void copy_construct(void *, const void *, allocator &) const override { std::terminate(); }
    std::size_t hash(const void *) const noexcept override { return 0; }
    void destroy(void *, allocator &) const noexcept override {}
    bool accepts_input(const ScalarRuntimeType &) const noexcept override { return false; }

  protected:
    void do_construct_from_input(void *, const ScalarData &, allocator &) const override { std::terminate(); }
    void do_assign_from_input(void *, const ScalarData &) const override { std::terminate(); }
};

template <template <template <typename, auto...> typename> typename Schema>
const FrameViewScalarRuntimeType<Schema> &frame_view_scalar_runtime_type() {
    static bg::util::static_storage<FrameViewScalarRuntimeType<Schema>> runtime;
    return runtime.get();
}

} // namespace details

class NativeFrameStorage {
  public:
    static constexpr bool supports_byte_copy_initialization = true;
    NativeFrameStorage() noexcept = default;
    NativeFrameStorage(const NativeFrameStorage &) = delete;

    void initialize(const details::NativeFrameLayout &layout) noexcept {
        layout_ = std::addressof(layout);
        auto *bytes = reinterpret_cast<std::byte *>(this);
        if (!layout.key_fields.empty())
            std::construct_at(std::addressof(key_state()));
        for (const auto &field : layout.fields) {
            field.construct(bytes + field.native_offset);
            std::construct_at(reinterpret_cast<TimeSeriesState *>(bytes + field.child_state_offset));
            field.construct_storage(bytes + field.column_storage_offset);
        }
    }

    const details::NativeFrameLayout &layout() const noexcept { return *layout_; }
    std::size_t size() const noexcept { return rows_.size(); }
    bool valid() const noexcept { return valid_; }
    bool contains(row_id_t id) const noexcept { return rows_.contains(id); }
    row_id_t id_at(std::size_t position) const { return rows_.id_at(position); }
    const void *row_domain() const noexcept { return rows_.identity(); }
    const util::index<> &added() const noexcept { return rows_.added(); }
    const util::index<> &removed() const noexcept { return rows_.removed(); }
    EvaluationTime change_time() const noexcept { return rows_.change_time(); }
    const NativeRows &rows() const noexcept { return rows_; }
    const util::index<> &changed_columns() const noexcept { return changed_columns_; }
    const util::index<> &changed() const noexcept { return changed_; }

    std::optional<row_id_t> find_key(std::span<const KeyValue> key) const {
        if (key.size() != layout_->key_fields.size())
            throw std::invalid_argument{"frame key width does not match its schema"};
        if (key.empty())
            return std::nullopt;

        const auto hash = hash_key(key);
        const auto *found = key_state().live_rows.find_with(hash, [&](row_id_t id) {
            return key_matches_row(id, key);
        });
        if (found == nullptr)
            return std::nullopt;
        return *found;
    }

    std::span<const KeyValue> removed_key(row_id_t id) const noexcept {
        if (layout_->key_fields.empty())
            return {};
        const auto *key = key_state().removed_rows.find(id);
        if (key == nullptr)
            return {};
        return *key;
    }

    void *native_field(std::size_t index) noexcept {
        return reinterpret_cast<std::byte *>(this) + layout_->fields[index].native_offset;
    }
    const void *native_field(std::size_t index) const noexcept {
        return reinterpret_cast<const std::byte *>(this) + layout_->fields[index].native_offset;
    }
    void *column_storage(std::size_t index) noexcept {
        return reinterpret_cast<std::byte *>(this) + layout_->fields[index].column_storage_offset;
    }
    const void *column_storage(std::size_t index) const noexcept {
        return reinterpret_cast<const std::byte *>(this) + layout_->fields[index].column_storage_offset;
    }
    const util::index<> *modified(std::size_t index) const noexcept {
        return layout_->fields[index].modified(native_field(index));
    }

    row_id_t add(std::span<const void *const> values, EvaluationTime time, allocator memory) {
        if (values.size() != layout_->fields.size())
            throw std::invalid_argument{"frame row field count does not match its schema"};
        const auto key_hash = layout_->key_fields.empty() ? std::size_t{} : hash_row_values(values);
        reserve_changed_columns(memory);
        begin_cycle(time, memory);
        const auto id = rows_.add(time, memory);
        const auto slot = rows_.slot_of(id);
        auto indexed = false;
        auto added_fields = std::size_t{};
        auto rollback = util::scope_exit{[&]() noexcept {
            if (indexed)
                key_state().live_rows.erase_with(key_hash, [id](row_id_t candidate) { return candidate == id; },
                                                 memory.memory_resource(), [](row_id_t *) noexcept {},
                                                 [](row_id_t *) noexcept {});
            for (std::size_t index = 0; index != added_fields; ++index)
                layout_->fields[index].erase(native_field(index), slot, id, memory);
            changed_.reset(id);
            rows_.rollback_add(id);
        }};
        changed_.set(id, memory.memory_resource());
        if (!layout_->key_fields.empty()) {
            const auto [stored_id, inserted] = key_state().live_rows.try_emplace_with(
                key_hash, [&](row_id_t candidate) { return key_matches_row_values(candidate, values); },
                memory.memory_resource(), [id](row_id_t *destination) { std::construct_at(destination, id); },
                [](row_id_t *value) noexcept { std::destroy_at(value); },
                [id](row_id_t *destination) { std::construct_at(destination, id); });
            if (!inserted)
                throw std::invalid_argument{"frame key is not unique"};
            indexed = true;
            static_cast<void>(stored_id);
        }
        for (std::size_t index = 0; index != layout_->fields.size(); ++index) {
            layout_->fields[index].add(native_field(index), rows_, id, values[index], memory);
            ++added_fields;
        }
        publish_all_columns(time, memory);
        valid_ = true;
        bind_column_storage();
        rollback.release();
        return id;
    }

    bool set(row_id_t id, std::span<const details::NativeFrameCellPatch> patches,
             EvaluationTime time, allocator memory) {
        if (patches.size() != layout_->fields.size())
            throw std::invalid_argument{"frame patch field count does not match its schema"};
        if (!rows_.contains(id))
            throw std::out_of_range{"frame row ID"};
        validate_key_patch(id, patches);
        reserve_changed_columns(memory);
        begin_cycle(time, memory);
        const auto has_change = std::ranges::any_of(patches, &details::NativeFrameCellPatch::changed);
        if (!has_change)
            return false;
        const auto was_changed = changed_.contains(id);
        if (!was_changed)
            changed_.set(id, memory.memory_resource());
        auto remove_empty_change = util::scope_exit{[&]() noexcept {
            if (!was_changed && !any_column_modified(id))
                changed_.reset(id);
        }};
        for (std::size_t index = 0; index != layout_->fields.size(); ++index) {
            if (!patches[index].changed)
                continue;
            const auto &field = layout_->fields[index];
            field.set(native_field(index), rows_, id, patches[index].value, memory);
            publish_column(index, time, memory);
        }
        valid_ = true;
        bind_column_storage();
        remove_empty_change.release();
        return true;
    }

    bool erase(row_id_t id, EvaluationTime time, allocator memory) {
        reserve_changed_columns(memory);
        begin_cycle(time, memory);
        if (!erase_row(id, time, memory))
            return false;
        publish_all_columns(time, memory);
        valid_ = true;
        bind_column_storage();
        return true;
    }

    void clear(EvaluationTime time, allocator memory) {
        reserve_changed_columns(memory);
        begin_cycle(time, memory);
        const auto was_valid = valid_;
        auto removed_rows = false;
        auto slot_iterator = rows_.live_slots().begin();
        while (slot_iterator != rows_.live_slots().end()) {
            const auto slot = *slot_iterator;
            ++slot_iterator;
            const auto id = rows_.id_at_slot(slot);
            if (!erase_row(id, time, memory))
                std::terminate();
            removed_rows = true;
        }
        if (!was_valid || removed_rows)
            publish_all_columns(time, memory);
        valid_ = true;
        bind_column_storage();
    }

    void invalidate(EvaluationTime time, allocator memory) {
        clear(time, memory);
        valid_ = false;
    }

    void release(allocator memory) noexcept {
        if (layout_ == nullptr)
            return;
        auto *bytes = reinterpret_cast<std::byte *>(this);
        for (const auto &field : layout_->fields)
            field.release(bytes + field.native_offset, memory);
        rows_.release(memory);
        if (!layout_->key_fields.empty()) {
            key_state().release(memory);
            std::destroy_at(std::addressof(key_state()));
        }
        changed_.release(memory.memory_resource());
        changed_columns_.release(memory.memory_resource());
        for (const auto &field : layout_->fields) {
            field.destroy_storage(bytes + field.column_storage_offset);
            std::destroy_at(reinterpret_cast<TimeSeriesState *>(bytes + field.child_state_offset));
            field.destroy(bytes + field.native_offset);
        }
        layout_ = nullptr;
        valid_ = false;
    }

    template <typename Function> void for_each_changed(Function &&function) const {
        for (auto id : changed_)
            std::invoke(function, id);
    }

    template <typename Function> void for_each_removed(Function &&function) const {
        for (auto id : rows_.removed())
            std::invoke(function, id);
    }

  private:
    bool erase_row(row_id_t id, EvaluationTime time, allocator memory) {
        if (!rows_.contains(id))
            return false;
        const auto slot = rows_.slot_of(id);
        const auto key_hash = layout_->key_fields.empty() ? std::size_t{} : hash_row(id);
        auto retained_key = false;
        if (!layout_->key_fields.empty() && !rows_.added().contains(id)) {
            auto [key, inserted] = key_state().removed_rows.try_emplace(
                id, memory.memory_resource(),
                std::pmr::polymorphic_allocator<KeyValue>{std::addressof(memory.memory_resource())});
            if (!inserted)
                throw std::logic_error{"removed frame key is already retained"};
            auto remove_partial_key = util::scope_exit{
                [&]() noexcept { key_state().removed_rows.erase(id, memory.memory_resource()); }};
            key->reserve(layout_->key_fields.size());
            append_row_key(id, *key);
            retained_key = true;
            remove_partial_key.release();
        }
        auto remove_retained_key = util::scope_exit{[&]() noexcept {
            if (retained_key)
                key_state().removed_rows.erase(id, memory.memory_resource());
        }};
        if (!rows_.erase(id, time, memory))
            return false;
        if (!layout_->key_fields.empty()) {
            const auto erased = key_state().live_rows.erase_with(
                key_hash, [id](row_id_t candidate) { return candidate == id; }, memory.memory_resource(),
                [](row_id_t *) noexcept {}, [](row_id_t *) noexcept {});
            if (!erased)
                std::terminate();
        }
        for (std::size_t index = 0; index != layout_->fields.size(); ++index)
            layout_->fields[index].erase(native_field(index), slot, id, memory);
        changed_.reset(id);
        remove_retained_key.release();
        return true;
    }
    details::NativeFrameKeyState &key_state() noexcept {
        return *reinterpret_cast<details::NativeFrameKeyState *>(
            reinterpret_cast<std::byte *>(this) + layout_->key_state_offset);
    }

    const details::NativeFrameKeyState &key_state() const noexcept {
        return *reinterpret_cast<const details::NativeFrameKeyState *>(
            reinterpret_cast<const std::byte *>(this) + layout_->key_state_offset);
    }

    static std::size_t combine_key_hash(std::size_t result, std::size_t value) noexcept {
        constexpr auto constant = std::size_t{0x9e3779b9};
        return result ^ (value + constant + (result << 6) + (result >> 2));
    }

    std::size_t hash_key(std::span<const KeyValue> key) const {
        auto result = std::size_t{};
        for (auto index = std::size_t{}; index != layout_->key_fields.size(); ++index) {
            const auto &field = layout_->fields[layout_->key_fields[index]];
            result = combine_key_hash(result, field.hash_erased_key(key[index]));
        }
        return result;
    }

    std::size_t hash_row(row_id_t id) const noexcept {
        auto result = std::size_t{};
        for (auto index : layout_->key_fields) {
            const auto &field = layout_->fields[index];
            result = combine_key_hash(result, field.hash_row_key(native_field(index), rows_, id));
        }
        return result;
    }

    std::size_t hash_row_values(std::span<const void *const> values) const noexcept {
        auto result = std::size_t{};
        for (auto index : layout_->key_fields) {
            const auto &field = layout_->fields[index];
            result = combine_key_hash(result, field.hash_key_value(values[index]));
        }
        return result;
    }

    bool key_matches_row(row_id_t id, std::span<const KeyValue> key) const {
        for (auto key_index = std::size_t{}; key_index != layout_->key_fields.size(); ++key_index) {
            const auto index = layout_->key_fields[key_index];
            const auto &field = layout_->fields[index];
            if (!field.equal_erased_row_key(native_field(index), rows_, id, key[key_index]))
                return false;
        }
        return true;
    }

    bool key_matches_row_values(row_id_t id, std::span<const void *const> values) const noexcept {
        for (auto index : layout_->key_fields) {
            const auto &field = layout_->fields[index];
            if (!field.equal_row_key(native_field(index), rows_, id, values[index]))
                return false;
        }
        return true;
    }

    void validate_key_patch(row_id_t id, std::span<const details::NativeFrameCellPatch> patches) const {
        for (auto index : layout_->key_fields) {
            if (patches[index].changed &&
                !layout_->fields[index].equal_row_key(native_field(index), rows_, id, patches[index].value))
                throw std::invalid_argument{"frame key columns are immutable"};
        }
    }

    void append_row_key(row_id_t id, std::pmr::vector<KeyValue> &key) const {
        for (auto index : layout_->key_fields) {
            const auto &field = layout_->fields[index];
            field.append_row_key(native_field(index), rows_, id, key);
        }
    }

    void reserve_changed_columns(allocator memory) {
        if (layout_->fields.size() > std::numeric_limits<util::index<>::size_type>::max())
            throw std::length_error{"frame schema has too many columns"};
        changed_columns_.grow(static_cast<util::index<>::size_type>(layout_->fields.size()),
                              memory.memory_resource());
    }

    void begin_cycle(EvaluationTime time, allocator memory) {
        if (rows_.change_time() != time && !layout_->key_fields.empty())
            key_state().removed_rows.release_with(
                memory.memory_resource(), [](row_id_t *) noexcept {},
                [](details::NativeFrameKeyState::KeyTuple *key) noexcept { std::destroy_at(key); });
        if (!rows_.reset_changes(time))
            return;
        changed_.clear();
        changed_columns_.clear();
        for (std::size_t index = 0; index != layout_->fields.size(); ++index)
            layout_->fields[index].reset(native_field(index));
    }

    void publish_column(std::size_t index, EvaluationTime time, allocator memory) {
        const auto &field = layout_->fields[index];
        auto *state = reinterpret_cast<std::byte *>(this) + field.child_state_offset;
        reinterpret_cast<TimeSeriesState *>(state)->publish(time);
        changed_columns_.set(static_cast<util::index<>::size_type>(index), memory.memory_resource());
    }

    void publish_all_columns(EvaluationTime time, allocator memory) {
        for (std::size_t index = 0; index != layout_->fields.size(); ++index)
            publish_column(index, time, memory);
    }

    void bind_column_storage() noexcept {
        for (std::size_t index = 0; index != layout_->fields.size(); ++index) {
            const auto &field = layout_->fields[index];
            field.bind_storage(column_storage(index), native_field(index), rows_);
        }
    }

    bool any_column_modified(row_id_t id) const noexcept {
        for (std::size_t index = 0; index != layout_->fields.size(); ++index)
            if (modified(index)->contains(id))
                return true;
        return false;
    }

    const details::NativeFrameLayout *layout_{};
    NativeRows rows_;
    util::index<> changed_;
    util::index<> changed_columns_;
    bool valid_{};
};

namespace details {

struct FrameSnapshotOwner {
    std::atomic_size_t references{1};
    const ScalarRuntimeType *runtime{};
    const NativeFrameLayout *layout{};
    NativeFrameStorage *frame{};
    bg::details::ScalarAllocationPool *allocation_pool{};
    allocator memory{};
    std::size_t size{};

    void retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept;
    static FrameSnapshotOwner *create(const NativeFrameLayout &layout, const FrameReadStorage &source,
                                      allocator memory);
};

inline FrameSnapshotOwner *frame_snapshot_owner(const ScalarData &source) noexcept {
    if (source.mode() == ScalarDataMode::shared_view || source.mode() == ScalarDataMode::shared_value)
        return reinterpret_cast<FrameSnapshotOwner *>(source.data_word());
    return static_cast<const FrameSnapshotStorage *>(source.data())->owner;
}

inline FrameColumnReadBinding native_frame_column_binding(const void *context, std::size_t index) {
    const auto &frame = *static_cast<const NativeFrameStorage *>(context);
    const auto &field = frame.layout().fields.at(index);
    return field.read_binding(frame.column_storage(index));
}

inline const util::index<> *native_frame_modified_column(const void *context, std::size_t index) noexcept {
    return static_cast<const NativeFrameStorage *>(context)->modified(index);
}

inline FrameReadStorage frame_read_storage(const NativeFrameStorage &frame, const void *owner,
                                           const RuntimeType *runtime, bool scalar_value = false) {
    auto result = FrameReadStorage{};
    result.schema = frame.layout().schema;
    result.rows = RowSet::native(frame.rows());
    result.valid = scalar_value || frame.valid();
    result.size = frame.size();
    result.added = std::addressof(frame.added());
    result.removed = std::addressof(frame.removed());
    result.changed = std::addressof(frame.changed());
    result.column_modified_context = std::addressof(frame);
    result.modified_column = &native_frame_modified_column;
    result.change_time = frame.change_time();
    result.columns_context = std::addressof(frame);
    result.column_binding = &native_frame_column_binding;
    result.changed_columns_context = std::addressof(frame.changed_columns());
    result.column_changed = &frame_column_changed;
    result.for_each_changed_column = &frame_for_each_changed_column;
    result.source_owner = owner == nullptr ? std::addressof(frame) : owner;
    result.source_runtime = runtime;
    return result;
}

inline FrameReadStorage frame_snapshot_read_storage(const NativeFrameStorage &frame, const void *owner,
                                                    const RuntimeType *runtime, std::size_t size) {
    auto result = FrameReadStorage{};
    result.schema = frame.layout().schema;
    result.valid = true;
    result.size = size;
    result.columns_context = std::addressof(frame);
    result.column_binding = &native_frame_column_binding;
    result.source_owner = owner == nullptr ? std::addressof(frame) : owner;
    result.source_runtime = runtime;
    return result;
}

inline void release_frame_snapshot_columns(NativeFrameStorage &frame, allocator memory) noexcept {
    for (std::size_t index = 0; index != frame.layout().fields.size(); ++index) {
        const auto &field = frame.layout().fields[index];
        field.release_snapshot_storage(frame.column_storage(index), memory);
    }
}

inline void copy_frame_snapshot_columns(NativeFrameStorage &destination, const FrameReadStorage &source,
                                        allocator memory) {
    const auto &layout = destination.layout();
    if (source.schema != layout.schema)
        throw std::invalid_argument{"frame snapshot schema does not match its runtime type"};
    auto snapshots = std::vector<void *>(layout.fields.size());
    auto release_snapshots = util::scope_exit{[&]() noexcept {
        for (std::size_t index = 0; index != snapshots.size(); ++index)
            if (snapshots[index] != nullptr)
                layout.fields[index].release_snapshot_column(snapshots[index]);
    }};
    for (std::size_t index = 0; index != layout.fields.size(); ++index)
        snapshots[index] = layout.fields[index].copy_snapshot_column(source.column(index), source.size, memory);

    for (std::size_t index = 0; index != layout.fields.size(); ++index) {
        const auto &field = layout.fields[index];
        field.release_snapshot_storage(destination.column_storage(index), memory);
        field.install_snapshot_column(destination.column_storage(index), snapshots[index]);
        snapshots[index] = nullptr;
    }
    release_snapshots.release();
}

inline FrameSnapshotOwner *FrameSnapshotOwner::create(const NativeFrameLayout &layout,
                                                      const FrameReadStorage &source, allocator memory) {
    if (source.schema != layout.schema)
        throw std::invalid_argument{"frame snapshot schema does not match its runtime type"};
    auto *allocation_pool = static_cast<bg::details::ScalarAllocationPool *>(nullptr);
    if (const auto *context = bg::details::current_scalar_allocation_owner()) {
        auto pool_memory = context->scalar_allocation_pool->memory();
        if (&memory.memory_resource() == &context->memory.memory_resource() ||
            &memory.memory_resource() == &pool_memory.memory_resource()) {
            allocation_pool = context->scalar_allocation_pool;
        }
    }
    if (allocation_pool != nullptr)
        allocation_pool->retain();
    auto release_allocation_pool = util::scope_exit{[&]() noexcept {
        if (allocation_pool != nullptr)
            allocation_pool->release();
    }};
    if (allocation_pool != nullptr)
        memory = allocation_pool->memory();

    auto *owner_memory = static_cast<FrameSnapshotOwner *>(
        memory.allocate_bytes(sizeof(FrameSnapshotOwner), alignof(FrameSnapshotOwner)));
    auto *owner = owner_memory;
    std::construct_at(owner);
    owner->memory = memory;
    owner->allocation_pool = allocation_pool;
    release_allocation_pool.release();
    try {
        owner->runtime = std::addressof(frame_snapshot_runtime_type(frame_type(*layout.schema)));
        owner->layout = std::addressof(layout);
        auto *allocation = memory.allocate_bytes(layout.size, layout.alignment);
        owner->frame = std::construct_at(reinterpret_cast<NativeFrameStorage *>(allocation));
        owner->frame->initialize(layout);
        copy_frame_snapshot_columns(*owner->frame, source, memory);
        owner->size = source.size;
    } catch (...) {
        if (owner->frame != nullptr)
            owner->layout = std::addressof(layout);
        owner->release();
        throw;
    }
    return owner;
}

inline void FrameSnapshotOwner::release() noexcept {
    if (references.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;

    auto resource = memory;
    auto *allocation_pool = std::exchange(this->allocation_pool, nullptr);
    if (frame != nullptr) {
        release_frame_snapshot_columns(*frame, resource);
        frame->release(resource);
        std::destroy_at(frame);
        resource.deallocate_bytes(frame, layout->size, layout->alignment);
    }
    std::destroy_at(this);
    resource.deallocate_bytes(this, sizeof(FrameSnapshotOwner), alignof(FrameSnapshotOwner));
    if (allocation_pool != nullptr)
        allocation_pool->release();
}

inline void copy_frame_snapshot_storage(FrameSnapshotStorage &destination, const NativeFrameLayout &layout,
                                        const FrameReadStorage &source, allocator memory) {
    auto *owner = FrameSnapshotOwner::create(layout, source, memory);
    auto *previous = std::exchange(destination.owner, owner);
    if (previous != nullptr)
        previous->release();
}

inline void copy_frame_snapshot_value(FrameSnapshotStorage &destination, const ScalarData &source,
                                      allocator memory) {
    const auto &runtime = static_cast<const FrameScalarRuntimeType &>(source.runtime_type());
    if (auto *owner = runtime.snapshot_owner(source)) {
        owner->retain();
        auto *previous = std::exchange(destination.owner, owner);
        if (previous != nullptr)
            previous->release();
        return;
    }

    const auto input = runtime.read_storage(source);
    if (input.schema == nullptr)
        throw std::invalid_argument{"frame scalar source has no schema"};
    copy_frame_snapshot_storage(destination, native_frame_layout(*input.schema), input, memory);
}

template <cell_type Cell> void bind_runtime_frame_column_view(const void *source, void *destination) {
    *static_cast<ColumnView<Cell> *>(destination) = static_cast<const ColumnView<> *>(source)->view<Cell>();
}

template <cell_type Cell>
void bind_frame_view_storage(void *destination, const FrameColumnReadBinding &source, RowSet visible_rows,
                             const util::index<> *added, const util::index<> *modified,
                             const util::index<> *removed, EvaluationTime time) {
    if (source.cell_type != std::addressof(cpp_scalar_type<Cell>()) || source.payload == nullptr ||
        source.copy_column_storage == nullptr)
        throw std::invalid_argument{"frame column type does not match the view schema"};
    source.copy_column_storage(destination, source.payload);
    auto &column = *static_cast<ColumnStorage<Cell> *>(destination);
    column.visible_rows = visible_rows;
    column.coverage = visible_rows;
    column.added = added;
    column.modified = modified;
    column.removed = removed;
    column.change_time = time;
}

template <cell_type Cell>
void bind_frame_computed_storage(void *destination, const void *native, const NativeRowSlots &physical_rows,
                                RowSet visible_rows, const util::index<> *added,
                                const util::index<> *modified, const util::index<> *removed,
                                EvaluationTime time) {
    *static_cast<ColumnStorage<Cell> *>(destination) = ColumnStorage<Cell>{
        *static_cast<const NativeColumnData<Cell> *>(native), physical_rows, visible_rows,
        *added, *modified, *removed, time};
}

inline FrameColumnReadBinding runtime_frame_column_binding(const ColumnView<> &column) {
    const auto &type = column.cell_type();
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>()))
        return {std::addressof(type), std::addressof(column), &bind_runtime_frame_column_view<i64>, nullptr};
    if (std::addressof(type) == std::addressof(cpp_scalar_type<double>()))
        return {std::addressof(type), std::addressof(column), &bind_runtime_frame_column_view<double>, nullptr};
    throw std::invalid_argument{"frame columns support only i64 and double cell types"};
}

} // namespace details

inline ScalarValue<Frame<bg::SCHEMA>> make_frame_value(allocator &memory, const bg::Schema &schema,
                                                       std::size_t row_count,
                                                       std::span<const ColumnView<>> columns);

inline ScalarValue<Frame<bg::SCHEMA>> make_frame_value(const bg::Schema &schema, std::size_t row_count,
                                                       std::span<const ColumnView<>> columns);

template <template <template <typename, auto...> typename> typename Schema>
class NativeFrame {
  public:
    static constexpr auto field_count = details::frame_schema_reflection<Schema>::field_count;
    using row_type = Row<Schema>;
    using patch_type = RowPatch<Schema>;

    NativeFrame() : layout_{std::addressof(details::native_frame_layout<Schema>())} {
        allocation_ = ::operator new(layout_->size, std::align_val_t{layout_->alignment});
        storage_ = std::construct_at(static_cast<NativeFrameStorage *>(allocation_));
        storage_->initialize(*layout_);
    }
    explicit NativeFrame(void *value) noexcept : storage_{static_cast<NativeFrameStorage *>(value)} {}
    NativeFrame(const NativeFrame &) = delete;
    NativeFrame &operator=(const NativeFrame &) = delete;
    ~NativeFrame() {
        if (allocation_ != nullptr) {
            std::destroy_at(storage_);
            ::operator delete(allocation_, std::align_val_t{layout_->alignment});
        }
    }

    std::size_t size() const noexcept { return storage_->size(); }
    bool valid() const noexcept { return storage_->valid(); }
    bool contains(row_id_t id) const noexcept { return storage_->contains(id); }
    row_id_t id_at(std::size_t position) const { return storage_->id_at(position); }
    const void *row_domain() const noexcept { return storage_->row_domain(); }
    const util::index<> &added() const noexcept { return storage_->added(); }
    const util::index<> &removed() const noexcept { return storage_->removed(); }
    EvaluationTime change_time() const noexcept { return storage_->change_time(); }
    const NativeRows &rows() const noexcept { return storage_->rows(); }
    const util::index<> &changed_columns() const noexcept { return storage_->changed_columns(); }
    NativeFrameStorage &core() const noexcept { return *storage_; }

    template <std::size_t Index> const auto &column() const noexcept {
        using Cell = details::frame_schema_cell_t<Index, Schema>;
        return *static_cast<const NativeColumnData<Cell> *>(storage_->native_field(Index));
    }
    template <std::size_t Index> auto cell(row_id_t id) const { return column<Index>().cell(rows(), id); }
    template <std::size_t Index> const util::index<> &modified() const noexcept {
        return *storage_->modified(Index);
    }

    FrameStorage<Schema> scalar_storage() const {
        return scalar_storage(std::make_index_sequence<field_count>{});
    }

    row_id_t add(const row_type &row, EvaluationTime time, allocator memory) {
        const auto values = add_values(row, std::make_index_sequence<field_count>{});
        return storage_->add(values, time, memory);
    }

    bool set(row_id_t id, const patch_type &patch, EvaluationTime time, allocator memory) {
        const auto fields = details::frame_schema_reflection<Schema>::fields(patch);
        const auto values = patch_values(fields, std::make_index_sequence<field_count>{});
        return storage_->set(id, values, time, memory);
    }

    bool erase(row_id_t id, EvaluationTime time, allocator memory) {
        return storage_->erase(id, time, memory);
    }

    void clear(EvaluationTime time, allocator memory) { storage_->clear(time, memory); }
    void invalidate(EvaluationTime time, allocator memory) { storage_->invalidate(time, memory); }
    void release(allocator memory) noexcept { storage_->release(memory); }

    template <typename Function> void for_each_changed(Function &&function) const {
        storage_->for_each_changed(std::forward<Function>(function));
    }
    template <typename Function> void for_each_removed(Function &&function) const {
        storage_->for_each_removed(std::forward<Function>(function));
    }

  private:
    template <std::size_t... Index> auto add_values(const row_type &row, std::index_sequence<Index...>) const {
        const auto fields = details::frame_schema_reflection<Schema>::fields(row);
        return std::array<const void *, sizeof...(Index)>{
            static_cast<const void *>(std::addressof(std::get<Index>(fields).value()))...};
    }

    template <typename Fields, std::size_t... Index>
    static auto patch_values(const Fields &fields, std::index_sequence<Index...>) {
        return std::array<details::NativeFrameCellPatch, sizeof...(Index)>{
            details::NativeFrameCellPatch{std::get<Index>(fields).changed(),
                                          std::addressof(std::get<Index>(fields).value())}...};
    }

    template <std::size_t... Index> FrameStorage<Schema> scalar_storage(std::index_sequence<Index...>) const {
        auto result = FrameStorage<Schema>{};
        auto destination = details::frame_schema_reflection<Schema>::fields(result.columns);
        ((std::get<Index>(destination) =
              *static_cast<const ColumnStorage<details::frame_schema_cell_t<Index, Schema>> *>(
                  storage_->column_storage(Index))),
         ...);
        result.rows = RowSet::native(storage_->rows());
        result.added = std::addressof(storage_->added());
        result.removed = std::addressof(storage_->removed());
        result.changed = std::addressof(storage_->changed());
        ((result.modified[Index] = storage_->modified(Index)), ...);
        result.changed_columns = std::addressof(storage_->changed_columns());
        result.change_time = storage_->change_time();
        result.valid = storage_->valid();
        result.size = storage_->size();
        return result;
    }

    const details::NativeFrameLayout *layout_{};
    NativeFrameStorage *storage_{};
    void *allocation_{};
};
namespace details {

template <cell_type Cell> NativeFrameFieldLayout make_native_frame_field_layout() {
    const auto &series_type = column_time_series_type<Cell>(TimeSeriesGuarantee::ordinary);
    auto result = NativeFrameFieldLayout{
        .cell_type = std::addressof(cpp_scalar_type<Cell>()),
        .column_runtime = std::addressof(column_view_time_series_runtime_type<Cell>(series_type)),
        .native_size = sizeof(NativeColumnData<Cell>),
        .native_alignment = alignof(NativeColumnData<Cell>),
        .child_state_size = sizeof(TimeSeriesState),
        .child_state_alignment = alignof(TimeSeriesState),
        .column_storage_size = sizeof(ColumnStorage<Cell>),
        .column_storage_alignment = alignof(ColumnStorage<Cell>),
        .add = [](void *field, NativeRows &rows, row_id_t id, const void *value, allocator memory) {
            static_cast<NativeColumnData<Cell> *>(field)->add(
                rows, id, *static_cast<const std::optional<Cell> *>(value), memory);
        },
        .set = [](void *field, NativeRows &rows, row_id_t id, const void *value, allocator memory) {
            static_cast<NativeColumnData<Cell> *>(field)->set(
                rows, id, *static_cast<const std::optional<Cell> *>(value), memory);
        },
        .erase = [](void *field, row_id_t slot, row_id_t id, allocator memory) {
            static_cast<NativeColumnData<Cell> *>(field)->erase_slot(slot, id, memory);
        },
        .reset = [](void *field) { static_cast<NativeColumnData<Cell> *>(field)->reset_changes(); },
        .release = [](void *field, allocator memory) { static_cast<NativeColumnData<Cell> *>(field)->release(memory); },
        .modified = [](const void *field) noexcept -> const util::index<> * {
            return std::addressof(static_cast<const NativeColumnData<Cell> *>(field)->modified());
        },
        .bind_storage = [](void *destination, const void *field, const NativeRows &rows) noexcept {
            *static_cast<ColumnStorage<Cell> *>(destination) =
                ColumnStorage<Cell>{*static_cast<const NativeColumnData<Cell> *>(field), rows};
        },
        .copy_column_storage = &copy_frame_column_storage<Cell>,
        .construct = [](void *field) noexcept { std::construct_at(static_cast<NativeColumnData<Cell> *>(field)); },
        .destroy = [](void *field) noexcept { std::destroy_at(static_cast<NativeColumnData<Cell> *>(field)); },
        .construct_storage = [](void *storage) noexcept {
            std::construct_at(static_cast<ColumnStorage<Cell> *>(storage));
        },
        .destroy_storage = [](void *storage) noexcept {
            std::destroy_at(static_cast<ColumnStorage<Cell> *>(storage));
        },
        .bind_column_view = &bind_frame_column_view<Cell>,
        .bind_column_series_view = &bind_frame_column_series_view<Cell>,
        .bind_view_storage = &bind_frame_view_storage<Cell>,
        .set_slots = [](void *field, const NativeRowSlots &rows, const util::index<> &added, row_id_t id,
                        const void *value, allocator memory) {
            return static_cast<NativeColumnData<Cell> *>(field)->set(
                rows, added, id, *static_cast<const std::optional<Cell> *>(value), memory);
        },
        .bind_computed_storage = &bind_frame_computed_storage<Cell>,
        .read_binding = &frame_column_read_binding<Cell>,
        .copy_snapshot_column = &copy_frame_snapshot_column<Cell>,
        .release_snapshot_column = &release_frame_snapshot_column<Cell>,
        .release_snapshot_storage = &release_frame_snapshot_storage<Cell>,
        .install_snapshot_column = &install_frame_snapshot_column<Cell>,
    };

    if constexpr (std::same_as<Cell, i64>) {
        result.hash_key_value = [](const void *value) noexcept {
            const auto &cell = *static_cast<const std::optional<Cell> *>(value);
            return cell ? std::hash<Cell>{}(*cell) : null_key_hash;
        };
        result.hash_row_key = [](const void *field, const NativeRows &rows, row_id_t id) noexcept {
            const auto cell = static_cast<const NativeColumnData<Cell> *>(field)->cell(rows, id);
            return cell ? std::hash<Cell>{}(*cell) : null_key_hash;
        };
        result.equal_row_key = [](const void *field, const NativeRows &rows, row_id_t id,
                                  const void *value) noexcept {
            const auto cell = static_cast<const NativeColumnData<Cell> *>(field)->cell(rows, id);
            const auto &candidate = *static_cast<const std::optional<Cell> *>(value);
            return cell == candidate;
        };
        result.append_row_key = [](const void *field, const NativeRows &rows, row_id_t id,
                                   std::pmr::vector<KeyValue> &key) {
            const auto cell = static_cast<const NativeColumnData<Cell> *>(field)->cell(rows, id);
            if (cell)
                key.emplace_back(*cell);
            else
                key.emplace_back(std::monostate{});
        };
        result.hash_erased_key = [](const KeyValue &key) {
            return std::visit(
                [](const auto &value) -> std::size_t {
                    using Value = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Value, std::monostate>)
                        return null_key_hash;
                    else
                        return std::hash<Value>{}(value);
                },
                key);
        };
        result.equal_erased_row_key = [](const void *field, const NativeRows &rows, row_id_t id,
                                         const KeyValue &key) {
            const auto cell = static_cast<const NativeColumnData<Cell> *>(field)->cell(rows, id);
            return std::visit(
                [&](const auto &value) {
                    using Value = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Value, std::monostate>)
                        return !cell.has_value();
                    else
                        return cell == value;
                },
                key);
        };
    }

    return result;
}

inline NativeFrameFieldLayout native_frame_field_layout(const ScalarType &cell_type) {
    if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>()))
        return make_native_frame_field_layout<i64>();
    if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<double>()))
        return make_native_frame_field_layout<double>();
    throw std::invalid_argument{"frame columns support only i64 and double cell types"};
}

} // namespace details

struct RuntimeFrameViewFieldLayout {
    details::NativeFrameFieldLayout column;
    std::size_t bookkeeping_offset{};
    std::size_t state_offset{};
    std::size_t column_storage_offset{};
    std::size_t native_offset{};
    bool computed{};
};

struct RuntimeFrameViewLayout {
    const bg::Schema *schema{};
    std::size_t size{};
    std::size_t alignment{};
    std::size_t computed_rows_offset{};
    bool has_computed_fields{};
    std::vector<RuntimeFrameViewFieldLayout> fields;
};

class RuntimeFrameViewOutputStorage {
  public:
    static constexpr bool supports_byte_copy_initialization = true;
    using predicate_function = bool (*)(void *, row_id_t);

    struct ColumnMapping {
        const void *native{};
        const NativeRows *rows{};
        const NativeRowSlots *physical_rows{};
        const void *snapshot{};
        const void *visible_context{};
        const void *visible_identity{};
        const void *coverage_context{};
        const void *coverage_identity{};
        friend bool operator==(const ColumnMapping &, const ColumnMapping &) = default;
    };

    struct FieldState {
        ColumnMapping mapping;
        util::index<> modified_rows;
    };

    const RuntimeFrameViewLayout *layout{};
    SelectedRows previous_rows;
    SelectedRows selected_rows;
    util::contiguous_bunch<row_id_t> selected_order;
    RowSet source_rows{};
    util::index<> changed_columns;
    util::index<> added_rows;
    util::index<> removed_rows;
    util::index<> changed_rows;
    util::index<> predicate_rows;
    EvaluationTime delta_time{MIN_DT};
    bool filtering{};
    bool valid{};

    void initialize(const RuntimeFrameViewLayout &runtime_layout) noexcept {
        layout = std::addressof(runtime_layout);
        if (layout->has_computed_fields)
            std::construct_at(computed_rows(this));
        for (std::size_t index = 0; index != layout->fields.size(); ++index)
            if (layout->fields[index].computed)
                layout->fields[index].column.construct(native_field(this, index));
    }

    bool is_computed(std::size_t index) const noexcept { return layout->fields[index].computed; }

    std::size_t field_index(util::symbol name) const {
        const auto fields = layout->schema->fields();
        for (auto index = std::size_t{}; index != fields.size(); ++index)
            if (fields[index].name == name)
                return index;
        throw std::invalid_argument{"frame view output field is absent from its resolved schema"};
    }

    FieldState &field_state(void *base, std::size_t index) const noexcept {
        auto *bytes = static_cast<std::byte *>(base);
        return *reinterpret_cast<FieldState *>(bytes + layout->fields[index].bookkeeping_offset);
    }
    const FieldState &field_state(const void *base, std::size_t index) const noexcept {
        const auto *bytes = static_cast<const std::byte *>(base);
        return *reinterpret_cast<const FieldState *>(bytes + layout->fields[index].bookkeeping_offset);
    }

    RowSet filter_rows() const noexcept {
        return RowSet{
            .context = this,
            .native_rows = nullptr,
            .identity = source_rows.identity,
            .size = [](const void *context) noexcept {
                return static_cast<std::size_t>(
                    static_cast<const RuntimeFrameViewOutputStorage *>(context)->selected_order.size());
            },
            .contains = [](const void *context, row_id_t id) noexcept {
                const auto &storage = *static_cast<const RuntimeFrameViewOutputStorage *>(context);
                return storage.source_rows.has(id) && storage.selected_rows.contains(id);
            },
            .id_at = [](const void *context, std::size_t position) {
                const auto &storage = *static_cast<const RuntimeFrameViewOutputStorage *>(context);
                if (position >= storage.selected_order.size())
                    throw std::out_of_range{"filtered row position"};
                return storage.selected_order[static_cast<util::contiguous_bunch<row_id_t>::size_type>(position)];
            },
            .for_each = [](const void *context, void *target, RowSet::visit_function visit) {
                const auto &storage = *static_cast<const RuntimeFrameViewOutputStorage *>(context);
                for (auto position = util::contiguous_bunch<row_id_t>::size_type{};
                     position != storage.selected_order.size(); ++position)
                    visit(target, storage.selected_order[position]);
            }};
    }

    void rebuild_selected_order(RowSet rows, allocator memory) {
        clear_selected_order(memory);
        rows.each([&](row_id_t id) {
            if (selected_rows.contains(id))
                selected_order.emplace_back(memory.memory_resource(), id);
        });
    }

    void clear_selected_order(allocator memory) noexcept {
        while (!selected_order.empty())
            selected_order.pop_back(memory.memory_resource());
    }
    TimeSeriesState &child_state(void *base, std::size_t index) const noexcept {
        auto *bytes = static_cast<std::byte *>(base);
        return *reinterpret_cast<TimeSeriesState *>(bytes + layout->fields[index].state_offset);
    }
    void *column_storage(void *base, std::size_t index) const noexcept {
        return static_cast<std::byte *>(base) + layout->fields[index].column_storage_offset;
    }
    const void *column_storage(const void *base, std::size_t index) const noexcept {
        return static_cast<const std::byte *>(base) + layout->fields[index].column_storage_offset;
    }

    void *native_field(void *base, std::size_t index) const noexcept {
        return static_cast<std::byte *>(base) + layout->fields[index].native_offset;
    }
    const void *native_field(const void *base, std::size_t index) const noexcept {
        return static_cast<const std::byte *>(base) + layout->fields[index].native_offset;
    }
    NativeRowSlots *computed_rows(void *base) const noexcept {
        return reinterpret_cast<NativeRowSlots *>(static_cast<std::byte *>(base) + layout->computed_rows_offset);
    }
    const NativeRowSlots *computed_rows(const void *base) const noexcept {
        return reinterpret_cast<const NativeRowSlots *>(static_cast<const std::byte *>(base) +
                                                        layout->computed_rows_offset);
    }

    bool set_computed(std::size_t index, row_id_t id, const void *value, EvaluationTime time, allocator memory) {
        if (!is_computed(index))
            throw std::invalid_argument{"frame output field is not computed"};
        if (!valid || !source_rows.has(id))
            throw std::out_of_range{"computed frame cell row ID"};
        if (value == nullptr)
            throw std::invalid_argument{"computed frame cell value is absent"};
        begin_cycle(time);
        const auto changed = write_computed(index, id, value, memory);
        finish_computed_updates(index, time, changed);
        return changed;
    }

    bool write_computed(std::size_t index, row_id_t id, const void *value, allocator memory) {
        const auto changed = layout->fields[index].column.set_slots(
            native_field(this, index), *computed_rows(this), added_rows, id, value, memory);
        if (!changed)
            return false;
        changed_rows.set(id, memory.memory_resource());
        changed_columns.set(index, memory.memory_resource());
        return true;
    }

    void finish_computed_updates(std::size_t index, EvaluationTime time, bool changed) {
        const auto &field = layout->fields[index];
        const auto *native_modified = field.column.modified(native_field(this, index));
        field.column.bind_computed_storage(
            column_storage(this, index), native_field(this, index), *computed_rows(this), source_rows,
            std::addressof(added_rows), native_modified, std::addressof(removed_rows), time);
        if (changed)
            child_state(this, index).publish(time);
    }

    bool bind(const FrameReadStorage &input, std::span<const std::size_t> source_indices,
              std::optional<std::size_t> predicate_index, void *predicate_context,
              predicate_function predicate, EvaluationTime time, allocator memory) {
        if (layout == nullptr || input.schema == nullptr)
            throw std::logic_error{"TSF view has no resolved schema layout"};
        if (valid && filtering != predicate_index.has_value())
            throw std::logic_error{"a TSF view output cannot switch between projection and filtering"};
        const auto infer_source_indices = source_indices.empty();
        const auto identity_source_indices = infer_source_indices && input.schema == layout->schema;
        if (!infer_source_indices && source_indices.size() != layout->fields.size())
            throw std::invalid_argument{"frame view column count does not match its output schema"};
        if (predicate_index.has_value() && (*predicate_index >= layout->fields.size() || predicate == nullptr))
            throw std::invalid_argument{"frame view filter has no resolved predicate"};

        const auto source_index = [&](std::size_t index) {
            if (!infer_source_indices)
                return source_indices[index];
            if (identity_source_indices)
                return index;
            return input.field_index(layout->schema->fields()[index].name);
        };
        const auto previous = previous_rows.source();
        const auto row_domain_changed = previous.identity != nullptr && previous.identity != input.rows.identity;
        const auto row_set_rebound = !valid || previous.context != input.rows.context;
        const auto input_delta_current = input.modified && input.change_time == input.last_updated;
        auto predicate_mapping_changed = false;
        for (std::size_t index = 0; index != layout->fields.size(); ++index) {
            if (is_computed(index))
                continue;
            const auto binding = input.column(source_index(index));
            const auto &field = layout->fields[index].column;
            if (binding.cell_type != field.cell_type || binding.payload == nullptr ||
                binding.copy_column_storage == nullptr || binding.native == nullptr ||
                binding.physical_rows == nullptr || binding.visible_rows.identity != input.rows.identity ||
                (binding.rows != nullptr && binding.rows->identity() != input.rows.identity))
                throw std::invalid_argument{"frame view column does not match the resolved schema or row domain"};
            const auto exact_native_coverage = binding.rows != nullptr &&
                                               std::addressof(binding.rows->slots()) == binding.physical_rows &&
                                               binding.coverage.context == input.rows.context &&
                                               binding.coverage.identity == input.rows.identity;
            const auto mapping = ColumnMapping{binding.native, binding.rows, binding.physical_rows, binding.snapshot,
                                               binding.visible_rows.context, binding.visible_rows.identity,
                                               binding.coverage.context, binding.coverage.identity};
            const auto mapping_changed = !valid || field_state(this, index).mapping != mapping;
            const auto exact_coverage = binding.coverage.context == input.rows.context &&
                                        binding.coverage.identity == input.rows.identity;
            if (!exact_native_coverage) {
                const auto validate_row = [&](row_id_t id) {
                    if (!binding.coverage.has(id) || !binding.physical_rows->contains(id))
                        throw std::invalid_argument{"frame view column does not cover every source row"};
                };
                const auto full_validation = !exact_coverage || mapping_changed || row_domain_changed ||
                                             row_set_rebound ||
                                             (input.modified && (!input_delta_current || input.added == nullptr));
                if (full_validation) {
                    input.rows.each(validate_row);
                } else if (input_delta_current) {
                    for (auto id : *input.added)
                        if (input.rows.has(id))
                            validate_row(id);
                }
            }
            if (predicate_index == index)
                predicate_mapping_changed = mapping_changed;
        }
        if (row_domain_changed)
            clear_computed_fields(memory);

        const auto new_cycle = !valid || delta_time != time;
        const auto rebuild_predicate = predicate_index.has_value() &&
                                       (!valid || row_set_rebound || row_domain_changed ||
                                        predicate_mapping_changed ||
                                        (input.modified && !input_delta_current));
        auto selected_order_dirty = predicate_index.has_value() && rebuild_predicate;
        if (new_cycle) {
            added_rows.clear();
            removed_rows.clear();
            changed_rows.clear();
            changed_columns.clear();
            for (std::size_t index = 0; index != layout->fields.size(); ++index) {
                field_state(this, index).modified_rows.clear();
                if (is_computed(index))
                    layout->fields[index].column.reset(native_field(this, index));
            }
            delta_time = time;
        }
        predicate_rows.clear();

        const auto clear_modified = [&](row_id_t id) {
            for (std::size_t index = 0; index != layout->fields.size(); ++index)
                field_state(this, index).modified_rows.reset(id);
        };
        const auto remove_filter_row = [&](row_id_t id, bool preserve_domain_removal) {
            if (!selected_rows.exclude(id))
                return;
            selected_order_dirty = true;
            remove_computed_row(id, memory);
            if (preserve_domain_removal || !added_rows.reset(id))
                removed_rows.set(id, memory.memory_resource());
            changed_rows.reset(id);
            clear_modified(id);
        };
        const auto add_filter_row = [&](row_id_t id, bool preserve_domain_addition) {
            if (!selected_rows.include(id, memory))
                return;
            selected_order_dirty = true;
            add_computed_row(id, memory);
            if (preserve_domain_addition || !removed_rows.reset(id))
                added_rows.set(id, memory.memory_resource());
            changed_rows.set(id, memory.memory_resource());
        };
        const auto remove_projection_row = [&](row_id_t id, bool preserve_domain_removal) {
            remove_computed_row(id, memory);
            if (preserve_domain_removal || !added_rows.reset(id))
                removed_rows.set(id, memory.memory_resource());
            changed_rows.reset(id);
            clear_modified(id);
        };
        const auto add_projection_row = [&](row_id_t id, bool preserve_domain_addition) {
            add_computed_row(id, memory);
            if (preserve_domain_addition || !removed_rows.reset(id))
                added_rows.set(id, memory.memory_resource());
            changed_rows.set(id, memory.memory_resource());
        };

        if (row_domain_changed) {
            if (predicate_index.has_value()) {
                selected_rows.for_each_selected([&](row_id_t id) { remove_filter_row(id, true); });
                selected_rows.clear();
            } else {
                previous_rows.for_each_selected([&](row_id_t id) { remove_projection_row(id, true); });
            }
            previous_rows.clear();
            input.rows.each([&](row_id_t id) {
                previous_rows.include(id, memory);
                if (predicate_index.has_value())
                    predicate_rows.set(id, memory.memory_resource());
                else
                    add_projection_row(id, true);
            });
        } else if (row_set_rebound || (input.modified && !input_delta_current)) {
            previous_rows.for_each_selected([&](row_id_t id) {
                if (!input.rows.has(id))
                    predicate_rows.set(id, memory.memory_resource());
            });
            for (auto id : predicate_rows) {
                if (predicate_index.has_value())
                    remove_filter_row(id, false);
                else
                    remove_projection_row(id, false);
                previous_rows.exclude(id);
            }
            predicate_rows.clear();
            input.rows.each([&](row_id_t id) {
                if (!previous_rows.contains(id)) {
                    if (predicate_index.has_value())
                        predicate_rows.set(id, memory.memory_resource());
                    else
                        add_projection_row(id, false);
                }
                previous_rows.include(id, memory);
            });
        } else if (input_delta_current) {
            if (input.removed != nullptr)
                for (auto id : *input.removed)
                    if (!input.rows.has(id) && previous_rows.contains(id)) {
                        if (predicate_index.has_value())
                            remove_filter_row(id, false);
                        else
                            remove_projection_row(id, false);
                        previous_rows.exclude(id);
                    }
            if (input.added != nullptr)
                for (auto id : *input.added)
                    if (input.rows.has(id) && !previous_rows.contains(id)) {
                        if (predicate_index.has_value())
                            predicate_rows.set(id, memory.memory_resource());
                        else
                            add_projection_row(id, false);
                        previous_rows.include(id, memory);
                    }
        }

        if (predicate_index.has_value()) {
            if (rebuild_predicate)
                input.rows.each([&](row_id_t id) { predicate_rows.set(id, memory.memory_resource()); });
            else if (input_delta_current)
                if (const auto *modified = input.modified_at(source_index(*predicate_index)); modified != nullptr)
                    for (auto id : *modified)
                        if (input.rows.has(id))
                            predicate_rows.set(id, memory.memory_resource());

            selected_rows.bind(input.rows);
            for (auto id : predicate_rows) {
                if (predicate(predicate_context, id)) {
                    add_filter_row(id, row_domain_changed);
                } else {
                    remove_filter_row(id, false);
                }
            }
            if (selected_order_dirty)
                rebuild_selected_order(input.rows, memory);
        }
        previous_rows.bind(input.rows);
        source_rows = input.rows;

        const auto output_rows = predicate_index.has_value() ? filter_rows() : input.rows;
        const auto membership_changed = !added_rows.empty() || !removed_rows.empty();
        auto any_change = !valid || row_domain_changed || membership_changed;
        for (std::size_t index = 0; index != layout->fields.size(); ++index) {
            auto &state = field_state(this, index);
            auto &output_modified = state.modified_rows;
            if (is_computed(index)) {
                state.mapping = {};
                const auto *native_modified = layout->fields[index].column.modified(native_field(this, index));
                for (auto id : *native_modified)
                    changed_rows.set(id, memory.memory_resource());
                if (!valid || row_domain_changed || !native_modified->empty() || membership_changed) {
                    changed_columns.set(index, memory.memory_resource());
                    child_state(this, index).publish(time);
                    any_change = true;
                }
                const auto &field = layout->fields[index];
                field.column.bind_computed_storage(
                    column_storage(this, index), native_field(this, index), *computed_rows(this), output_rows,
                    std::addressof(added_rows), native_modified, std::addressof(removed_rows), time);
                continue;
            }
            const auto binding = input.column(source_index(index));
            const auto mapping = ColumnMapping{binding.native, binding.rows, binding.physical_rows, binding.snapshot,
                                               binding.visible_rows.context, binding.visible_rows.identity,
                                               binding.coverage.context, binding.coverage.identity};
            const auto mapping_changed = !valid || state.mapping != mapping;
            state.mapping = mapping;
            const auto *source_modified = input_delta_current ? input.modified_at(source_index(index)) : nullptr;
            if (mapping_changed || (input.modified && !input_delta_current)) {
                output_rows.each([&](row_id_t id) {
                    if (!added_rows.contains(id))
                        output_modified.set(id, memory.memory_resource());
                });
            } else if (source_modified != nullptr) {
                for (auto id : *source_modified)
                    if (output_rows.has(id) && !added_rows.contains(id))
                        output_modified.set(id, memory.memory_resource());
            }
            for (auto id : output_modified)
                changed_rows.set(id, memory.memory_resource());
            if (mapping_changed || !output_modified.empty() || membership_changed) {
                changed_columns.set(index, memory.memory_resource());
                auto *bytes = reinterpret_cast<std::byte *>(this);
                reinterpret_cast<TimeSeriesState *>(bytes + layout->fields[index].state_offset)->publish(time);
                any_change = true;
            }
            layout->fields[index].column.bind_view_storage(
                column_storage(this, index), binding, output_rows, std::addressof(added_rows),
                std::addressof(output_modified), std::addressof(removed_rows), time);
        }

        filtering = predicate_index.has_value();
        valid = true;
        return any_change;
    }

    void invalidate(allocator memory) noexcept {
        clear_computed_fields(memory);
        previous_rows.clear();
        previous_rows.bind({});
        selected_rows.clear();
        selected_rows.bind({});
        clear_selected_order(memory);
        source_rows = {};
        added_rows.clear();
        removed_rows.clear();
        changed_rows.clear();
        predicate_rows.clear();
        changed_columns.clear();
        for (std::size_t index = 0; index != layout->fields.size(); ++index) {
            auto &field = field_state(this, index);
            field.modified_rows.clear();
            field.mapping = {};
            if (is_computed(index))
                layout->fields[index].column.reset(native_field(this, index));
        }
        valid = false;
    }

    void release(allocator memory) noexcept {
        previous_rows.release(memory);
        selected_rows.release(memory);
        selected_order.release(memory.memory_resource());
        added_rows.release(memory.memory_resource());
        removed_rows.release(memory.memory_resource());
        changed_rows.release(memory.memory_resource());
        predicate_rows.release(memory.memory_resource());
        changed_columns.release(memory.memory_resource());
        for (std::size_t index = 0; index != layout->fields.size(); ++index) {
            auto &field = field_state(this, index);
            field.modified_rows.release(memory.memory_resource());
            field.mapping = {};
            if (is_computed(index))
                layout->fields[index].column.release(native_field(this, index), memory);
        }
        if (layout->has_computed_fields)
            computed_rows(this)->release(memory);
        source_rows = {};
        valid = false;
    }

    void begin_cycle(EvaluationTime time) {
        if (delta_time == time)
            return;
        added_rows.clear();
        removed_rows.clear();
        changed_rows.clear();
        changed_columns.clear();
        for (std::size_t index = 0; index != layout->fields.size(); ++index) {
            field_state(this, index).modified_rows.clear();
            if (is_computed(index))
                layout->fields[index].column.reset(native_field(this, index));
        }
        delta_time = time;
    }

    void clear_computed_fields(allocator memory) noexcept {
        if (!layout->has_computed_fields)
            return;
        auto &rows = *computed_rows(this);
        auto slot_iterator = rows.live_slots().begin();
        while (slot_iterator != rows.live_slots().end()) {
            const auto slot = *slot_iterator;
            ++slot_iterator;
            const auto id = rows.id_at_slot(slot);
            for (std::size_t index = 0; index != layout->fields.size(); ++index)
                if (is_computed(index))
                    layout->fields[index].column.erase(native_field(this, index), slot, id, memory);
            rows.erase(id);
        }
    }

    void remove_computed_row(row_id_t id, allocator memory) {
        if (!layout->has_computed_fields)
            return;
        auto &rows = *computed_rows(this);
        if (!rows.contains(id))
            return;
        const auto slot = rows.slot_of(id);
        for (std::size_t index = 0; index != layout->fields.size(); ++index)
            if (is_computed(index))
                layout->fields[index].column.erase(native_field(this, index), slot, id, memory);
        rows.erase(id);
    }

    void add_computed_row(row_id_t id, allocator memory) {
        if (!layout->has_computed_fields)
            return;
        auto &rows = *computed_rows(this);
        if (!rows.contains(id))
            rows.insert(id, memory);
    }
};

template <typename Cell, template <template <typename, auto...> typename> typename Schema, auto... Properties>
auto select_column(wp<TSF<Schema, Properties...>> source, util::symbol name) {
    const auto *frame_type = dynamic_cast<const frame::FrameSeriesType *>(std::addressof(source.type()));
    if (frame_type == nullptr)
        throw std::invalid_argument{"frame column selection requires a frame time series"};

    const auto *schema = dynamic_cast<const bg::Schema *>(std::addressof(frame_type->schema_type()));
    if (schema == nullptr)
        throw std::invalid_argument{"frame column selection requires a resolved frame schema"};
    const auto *field = schema->find(name);
    if (field == nullptr)
        throw std::out_of_range{"frame column selection names an unknown field"};
    const auto fields = schema->fields();
    const auto index = static_cast<std::size_t>(field - fields.data());

    const auto child = WiringContext::current().wiring().output_item(
        static_cast<WiringPoint>(source), static_cast<item_id_t>(index), frame_type->item_type(index));
    return child.template as<TSC<Cell>>();
}

template <typename Cell, template <template <typename, auto...> typename> typename Schema, auto... Properties>
auto select_column(wp<TSF<Schema, Properties...>> source, std::size_t index) {
    const auto *frame_type = dynamic_cast<const frame::FrameSeriesType *>(std::addressof(source.type()));
    if (frame_type == nullptr)
        throw std::invalid_argument{"frame column selection requires a frame time series"};
    const auto *schema = dynamic_cast<const bg::Schema *>(std::addressof(frame_type->schema_type()));
    if (schema == nullptr)
        throw std::invalid_argument{"frame column selection requires a resolved frame schema"};
    if (index >= schema->field_count())
        throw std::out_of_range{"frame column selection index is outside the resolved schema"};
    return select_column<Cell>(source, schema->field(index).name);
}

template <template <template <typename, auto...> typename> typename Schema, auto... Properties,
          typename... Arguments>
    requires(bg::details::graph_code_argument<Arguments> && ...)
wp<> bind_expression(wp<TSF<Schema, Properties...>> source, const Expression &expression,
                     Arguments &&...arguments) {
    auto bindings = bg::details::make_wiring_arguments(std::forward<Arguments>(arguments)...);
    const auto column_resolver = [source](util::symbol name) {
        return WiringArgument{select_column<SCALAR>(source, name)};
    };
    return wp<>{details::bind_expression(expression, bindings, column_resolver)};
}

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema, auto... Properties>
auto select_column(wp<TSF<Schema, Properties...>> source) {
    if constexpr (bg::details::schema_variable_template<Schema>) {
        return select_column<SCALAR>(source, Index);
    } else {
        static_assert(Index < details::frame_schema_reflection<Schema>::field_count,
                      "frame column index is outside the declared schema");
        using Cell = details::frame_schema_cell_t<Index, Schema>;
        return select_column<Cell>(source, details::frame_declared_field_symbol<Index, Schema>());
    }
}

template <util::compile_time_string Name,
          template <template <typename, auto...> typename> typename Schema, auto... Properties>
auto select_column(wp<TSF<Schema, Properties...>> source) {
    if constexpr (bg::details::schema_variable_template<Schema>) {
        return select_column<SCALAR>(source, util::symbol{Name.view()});
    } else {
        constexpr auto index = details::frame_column_index<Name, Schema>();
        static_assert(index < details::frame_schema_reflection<Schema>::field_count,
                      "unknown frame column name");
        return select_column<index>(source);
    }
}

namespace details {

class NativeFrameSeriesRuntimeType final : public bg::frame::FrameSeriesRuntimeType {
  public:
    using ItemLayout = TimeSeriesRuntimeType::ItemLayout;
    using ConstItem = TimeSeriesRuntimeType::ConstItem;
    using MutableItem = TimeSeriesRuntimeType::MutableItem;
    using ItemIdFunction = TimeSeriesRuntimeType::ItemIdFunction;
    using ModifiedItemFunction = TimeSeriesRuntimeType::ModifiedItemFunction;

    explicit NativeFrameSeriesRuntimeType(const TimeSeriesType &type)
        : bg::frame::FrameSeriesRuntimeType{
              type,
              native_frame_layout(static_cast<const FrameSeriesType &>(type).schema_type()).size,
              native_frame_layout(static_cast<const FrameSeriesType &>(type).schema_type()).alignment, true, true,
              true},
          layout_{std::addressof(native_frame_layout(static_cast<const FrameSeriesType &>(type).schema_type()))} {
        fields_.reserve(layout_->fields.size());
        for (const auto &field : layout_->fields)
            fields_.emplace_back(*field.column_runtime, field.child_state_offset, field.column_storage_offset);
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
        const auto &frame = *static_cast<const NativeFrameStorage *>(value);
        return last_updated.valid() && frame.change_time() == last_updated.time() &&
               frame.changed_columns().contains(id);
    }
    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &frame = *static_cast<const NativeFrameStorage *>(value);
        if (!last_updated.valid() || frame.change_time() != last_updated.time())
            return;
        for (auto id : frame.changed_columns())
            function(context, id);
    }
    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &frame = *static_cast<const NativeFrameStorage *>(value);
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
        return static_cast<const NativeFrameStorage *>(value)->valid();
    }

    FrameReadStorage input_storage(const void *value) const noexcept override {
        return frame_read_storage(*static_cast<const NativeFrameStorage *>(value), value, this);
    }

    void construct_initial_value(void *destination) const noexcept override {
        auto *frame = std::construct_at(static_cast<NativeFrameStorage *>(destination));
        frame->initialize(*layout_);
    }
    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto *frame = static_cast<NativeFrameStorage *>(value);
        frame->release(memory);
        std::destroy_at(frame);
    }
    void invalidate_output(const OutputState &output) const override {
        auto *frame = static_cast<NativeFrameStorage *>(output.mutable_data());
        frame->invalidate(output.context().evaluation_time(), output.allocator());
        this->invalidate_output_items(output);
        this->invalidate_output_value(output);
    }

  private:
    const NativeFrameLayout *layout_;
    std::vector<ItemLayout> fields_;
};

class RuntimeSchemaFrameViewSeriesRuntimeType final : public FrameSeriesRuntimeType {
  public:
    using Storage = RuntimeFrameViewOutputStorage;
    using ItemLayout = TimeSeriesRuntimeType::ItemLayout;
    using ConstItem = TimeSeriesRuntimeType::ConstItem;
    using MutableItem = TimeSeriesRuntimeType::MutableItem;
    using ItemIdFunction = TimeSeriesRuntimeType::ItemIdFunction;
    using ModifiedItemFunction = TimeSeriesRuntimeType::ModifiedItemFunction;

    explicit RuntimeSchemaFrameViewSeriesRuntimeType(const TimeSeriesType &type,
                                                     std::vector<std::size_t> computed_fields = {})
        : FrameSeriesRuntimeType{type, make_layout(type, computed_fields).size,
                                 make_layout(type, computed_fields).alignment, true, true, true},
          layout_{make_layout(type, computed_fields)}, computed_fields_{std::move(computed_fields)} {
        for (const auto &field : layout_.fields)
            fields_.emplace_back(*field.column.column_runtime, field.state_offset, field.column_storage_offset);
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
        return last_updated.valid() && storage.delta_time == last_updated.time() &&
               storage.changed_columns.contains(id);
    }
    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &storage = *static_cast<const Storage *>(value);
        if (!last_updated.valid() || storage.delta_time != last_updated.time())
            return;
        for (auto id : storage.changed_columns)
            function(context, id);
    }
    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value,
                                EvaluationTime evaluation_time, void *context,
                                ModifiedItemFunction function) const override {
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
        if (storage.delta_time != evaluation_time)
            return;
        for (auto id : storage.changed_columns)
            publish(id);
    }
    bool collection_valid(const void *value) const noexcept override {
        return static_cast<const Storage *>(value)->valid;
    }
    FrameReadStorage input_storage(const void *value) const noexcept override {
        const auto &storage = *static_cast<const Storage *>(value);
        auto result = FrameReadStorage{};
        result.schema = layout_.schema;
        result.rows = storage.valid
                          ? (storage.filtering ? storage.filter_rows() : storage.source_rows)
                          : RowSet{};
        result.valid = storage.valid;
        result.modified = storage.delta_time != MIN_DT && !storage.changed_columns.empty();
        result.size = result.rows.count();
        result.added = std::addressof(storage.added_rows);
        result.removed = std::addressof(storage.removed_rows);
        result.changed = std::addressof(storage.changed_rows);
        result.column_modified_context = std::addressof(storage);
        result.modified_column = &modified_column;
        result.change_time = storage.delta_time;
        result.last_updated = storage.delta_time;
        result.columns_context = value;
        result.column_binding = &column_binding;
        result.changed_columns_context = std::addressof(storage.changed_columns);
        result.column_changed = &frame_column_changed;
        result.for_each_changed_column = &frame_for_each_changed_column;
        result.source_owner = value;
        result.source_runtime = this;
        return result;
    }
    void construct_initial_value(void *destination) const noexcept override {
        auto *storage = std::construct_at(static_cast<Storage *>(destination));
        storage->initialize(layout_);
        for (std::size_t index = 0; index != layout_.fields.size(); ++index) {
            const auto &field = layout_.fields[index].column;
            auto *bytes = static_cast<std::byte *>(destination);
            std::construct_at(reinterpret_cast<Storage::FieldState *>(bytes + layout_.fields[index].bookkeeping_offset));
            std::construct_at(reinterpret_cast<TimeSeriesState *>(bytes + layout_.fields[index].state_offset));
            field.construct_storage(bytes + layout_.fields[index].column_storage_offset);
        }
    }
    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto &storage = *static_cast<Storage *>(value);
        storage.release(memory);
        auto *bytes = static_cast<std::byte *>(value);
        for (std::size_t index = layout_.fields.size(); index != 0; --index) {
            const auto &field = layout_.fields[index - 1];
            field.column.destroy_storage(bytes + field.column_storage_offset);
            std::destroy_at(reinterpret_cast<TimeSeriesState *>(bytes + field.state_offset));
            std::destroy_at(reinterpret_cast<Storage::FieldState *>(bytes + field.bookkeeping_offset));
        }
        std::destroy_at(&storage);
    }
    void invalidate_output(const OutputState &output) const override {
        auto &storage = *static_cast<Storage *>(output.mutable_data());
        storage.invalidate(output.allocator());
        this->invalidate_output_items(output);
        this->invalidate_output_value(output);
    }

  private:
    static RuntimeFrameViewLayout make_layout(const TimeSeriesType &type,
                                              std::span<const std::size_t> computed_fields) {
        const auto *series = dynamic_cast<const FrameSeriesType *>(std::addressof(type));
        if (series == nullptr || !series->is_resolved() ||
            series->guarantee().value() != TimeSeriesGuarantee::ordinary)
            throw std::invalid_argument{"runtime-schema TSF view requires a resolved ordinary type"};
        const auto &native = native_frame_layout(series->schema_type());
        auto result = RuntimeFrameViewLayout{};
        result.schema = native.schema;
        result.alignment = alignof(Storage);
        auto offset = sizeof(Storage);
        result.has_computed_fields = !computed_fields.empty();
        if (result.has_computed_fields) {
            result.alignment = std::max(result.alignment, alignof(NativeRowSlots));
            offset = (offset + alignof(NativeRowSlots) - 1) / alignof(NativeRowSlots) * alignof(NativeRowSlots);
            result.computed_rows_offset = offset;
            offset += sizeof(NativeRowSlots);
        }
        result.fields.reserve(native.fields.size());
        for (std::size_t index = 0; index != native.fields.size(); ++index) {
            const auto &column = native.fields[index];
            const auto is_computed = std::ranges::find(computed_fields, index) != computed_fields.end();
            const auto align = [&](std::size_t alignment) {
                result.alignment = std::max(result.alignment, alignment);
                offset = (offset + alignment - 1) / alignment * alignment;
            };
            auto native_offset = std::size_t{};
            if (is_computed) {
                align(column.native_alignment);
                native_offset = offset;
                offset += column.native_size;
            }
            align(alignof(Storage::FieldState));
            const auto bookkeeping_offset = offset;
            offset += sizeof(Storage::FieldState);
            align(alignof(TimeSeriesState));
            const auto state_offset = offset;
            offset += sizeof(TimeSeriesState);
            align(column.column_storage_alignment);
            const auto column_storage_offset = offset;
            offset += column.column_storage_size;
            result.fields.push_back(
                {column, bookkeeping_offset, state_offset, column_storage_offset, native_offset, is_computed});
        }
        result.size = (offset + result.alignment - 1) / result.alignment * result.alignment;
        return result;
    }

    static const util::index<> *modified_column(const void *context, std::size_t index) noexcept {
        const auto &storage = *static_cast<const Storage *>(context);
        if (index >= storage.layout->fields.size())
            return nullptr;
        const auto &field = storage.layout->fields[index];
        if (field.computed)
            return field.column.modified(storage.native_field(context, index));
        return std::addressof(storage.field_state(context, index).modified_rows);
    }
    static FrameColumnReadBinding column_binding(const void *context, std::size_t index) {
        const auto &storage = *static_cast<const Storage *>(context);
        const auto &field = storage.layout->fields.at(index).column;
        const auto *bytes = static_cast<const std::byte *>(context);
        return field.read_binding(storage.column_storage(bytes, index));
    }

    RuntimeFrameViewLayout layout_;
    std::vector<std::size_t> computed_fields_;
    std::vector<ItemLayout> fields_;

  public:
    const std::vector<std::size_t> &computed_fields() const noexcept { return computed_fields_; }
};

class FrameSeriesScalarRuntimeType final : public FrameScalarRuntimeType {
  public:
    FrameSeriesScalarRuntimeType(const FrameSeriesRuntimeType &series_runtime, const FrameType &frame_type)
        : FrameScalarRuntimeType{frame_type, series_runtime.value_size(), series_runtime.value_alignment(), false,
                                 false, false},
          series_runtime_{std::addressof(series_runtime)} {}

    FrameReadStorage read_storage(const ScalarData &source) const override {
        return series_runtime_->input_storage(source.data());
    }
    void read_value(ScalarDataMode, std::uintptr_t, void *) const override { std::terminate(); }
    void construct_initial_value(void *) const noexcept override { std::terminate(); }
    void copy_construct(void *, const void *, allocator &) const override { std::terminate(); }
    std::size_t hash(const void *) const noexcept override { return 0; }
    void destroy(void *, allocator &) const noexcept override {}
    bool accepts_input(const ScalarRuntimeType &) const noexcept override { return false; }

  protected:
    void do_construct_from_input(void *, const ScalarData &, allocator &) const override { std::terminate(); }
    void do_assign_from_input(void *, const ScalarData &) const override { std::terminate(); }

  private:
    const FrameSeriesRuntimeType *series_runtime_;
};

class FrameSnapshotRuntimeType final : public FrameScalarRuntimeType {
  public:
    explicit FrameSnapshotRuntimeType(const FrameType &type)
        : FrameScalarRuntimeType{type, sizeof(FrameSnapshotStorage), alignof(FrameSnapshotStorage), false, true,
                                 true, shared_handle_flag} {}

    FrameReadStorage read_storage(const ScalarData &source) const override {
        const auto *owner = frame_snapshot_owner(source);
        if (owner == nullptr || owner->frame == nullptr)
            throw std::logic_error{"frame scalar snapshot has no value"};
        return frame_snapshot_read_storage(*owner->frame, owner->frame, owner->runtime, owner->size);
    }
    FrameSnapshotOwner *snapshot_owner(const ScalarData &source) const noexcept override {
        return frame_snapshot_owner(source);
    }
    void read_value(ScalarDataMode, std::uintptr_t, void *) const override { std::terminate(); }
    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<FrameSnapshotStorage *>(destination));
    }
    void copy_construct(void *destination, const void *source, allocator &memory) const override {
        const auto input = ScalarData{*this, source};
        do_construct_from_input(destination, input, memory);
    }
    std::size_t hash(const void *) const noexcept override { return 0; }
    void destroy(void *value, allocator &memory) const noexcept override {
        static_cast<void>(memory);
        auto *storage = static_cast<FrameSnapshotStorage *>(value);
        if (auto *owner = std::exchange(storage->owner, nullptr))
            owner->release();
        std::destroy_at(storage);
    }
    const ScalarRuntimeType *retain_shared(const ScalarData &source, std::uintptr_t &data) const noexcept override {
        auto *owner = frame_snapshot_owner(source);
        if (owner == nullptr)
            return nullptr;
        owner->retain();
        data = reinterpret_cast<std::uintptr_t>(owner);
        return owner->runtime;
    }
    void release_shared(std::uintptr_t data) const noexcept override {
        if (auto *owner = reinterpret_cast<FrameSnapshotOwner *>(data))
            owner->release();
    }
    bool accepts_input(const ScalarRuntimeType &source) const noexcept override {
        return type().accepts(source.type());
    }

  protected:
    void do_construct_from_input(void *destination, const ScalarData &source, allocator &memory) const override {
        auto *storage = std::construct_at(static_cast<FrameSnapshotStorage *>(destination));
        try {
            copy_frame_snapshot_value(*storage, source, memory);
        } catch (...) {
            if (auto *owner = std::exchange(storage->owner, nullptr))
                owner->release();
            std::destroy_at(storage);
            throw;
        }
    }
    void do_assign_from_input(void *destination, const ScalarData &source) const override {
        copy_frame_snapshot_value(*static_cast<FrameSnapshotStorage *>(destination), source,
                                  bg::details::current_scalar_allocator());
    }

    ScalarData borrow_shared(const void *value) const noexcept override {
        auto *owner = static_cast<const FrameSnapshotStorage *>(value)->owner;
        if (owner == nullptr)
            return ScalarData{*this, value};
        return ScalarData{*owner->runtime, reinterpret_cast<std::uintptr_t>(owner), ScalarDataMode::shared_view};
    }
};

} // namespace details

template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesType &frame_time_series_type(TimeSeriesGuarantee guarantee) {
    if (guarantee != TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"TSF requires ordinary"};
    const auto &type = frame_series_type(frame_schema_type<Schema>(), guarantee);
    if (type.is_resolved()) {
        const auto &runtime = details::canonical_frame_series_runtime_type(
            type,
            [](const TimeSeriesType &series_type) -> std::unique_ptr<TimeSeriesRuntimeType> {
                return std::make_unique<details::NativeFrameSeriesRuntimeType>(series_type);
            });
        register_time_series_runtime_type(runtime);
        const auto &frame_runtime = static_cast<const FrameSeriesRuntimeType &>(runtime);
        frame_series_scalar_runtime_type(frame_runtime);
    }
    return type;
}

template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesRuntimeType &frame_time_series_runtime_type(const TimeSeriesType &type) {
    const auto *frame_type = dynamic_cast<const FrameSeriesType *>(std::addressof(type));
    if (frame_type == nullptr || !frame_type->is_resolved() ||
        frame_type->guarantee().value() != TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"TSF runtime requires a resolved ordinary frame series type"};
    const auto &runtime = details::canonical_frame_series_runtime_type(
        type, [](const TimeSeriesType &series_type) -> std::unique_ptr<TimeSeriesRuntimeType> {
            return std::make_unique<details::NativeFrameSeriesRuntimeType>(series_type);
        });
    register_time_series_runtime_type(runtime);
    const auto &frame_runtime = static_cast<const FrameSeriesRuntimeType &>(runtime);
    frame_series_scalar_runtime_type(frame_runtime);
    return runtime;
}

namespace details {
const TimeSeriesRuntimeType &canonical_frame_view_runtime_type(const TimeSeriesType &type,
                                                               std::vector<std::size_t> computed_fields);
}

template <template <template <typename, auto...> typename> typename Schema>
const TimeSeriesRuntimeType &frame_view_time_series_runtime_type(const TimeSeriesType &type) {
    const auto *series = dynamic_cast<const FrameSeriesType *>(std::addressof(type));
    if (series == nullptr || !series->is_resolved() ||
        series->guarantee().value() != TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"TSF view runtime requires a resolved ordinary type"};
    return details::canonical_frame_view_runtime_type(type, {});
}

namespace details {

struct FrameViewRuntimeCache {
    std::mutex mutex;
    std::unordered_map<const TimeSeriesType *,
                       std::vector<std::unique_ptr<RuntimeSchemaFrameViewSeriesRuntimeType>>> runtimes;
};

inline FrameViewRuntimeCache &frame_view_runtime_cache() {
    static bg::util::static_storage<FrameViewRuntimeCache> cache;
    return cache.get();
}

inline const TimeSeriesRuntimeType &canonical_frame_view_runtime_type(
    const TimeSeriesType &type, std::vector<std::size_t> computed_fields) {
    auto &cache = frame_view_runtime_cache();
    const auto lock = std::lock_guard{cache.mutex};
    auto &candidates = cache.runtimes[std::addressof(type)];
    for (const auto &candidate : candidates)
        if (candidate->computed_fields() == computed_fields) {
            frame_series_scalar_runtime_type(*candidate);
            return *candidate;
        }
    candidates.push_back(std::make_unique<RuntimeSchemaFrameViewSeriesRuntimeType>(type, std::move(computed_fields)));
    frame_series_scalar_runtime_type(*candidates.back());
    return *candidates.back();
}

} // namespace details

template <template <template <typename, auto...> typename> typename Schema,
          util::compile_time_string... Names>
const TimeSeriesRuntimeType &frame_computed_time_series_runtime_type(const TimeSeriesType &type,
                                                                     computed_option<Names...>) {
    const auto *series = dynamic_cast<const FrameSeriesType *>(std::addressof(type));
    if (series == nullptr || !series->is_resolved() ||
        series->guarantee().value() != TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"computed TSF runtime requires a resolved ordinary type"};
    const auto &schema = static_cast<const bg::Schema &>(series->schema_type());
    auto indices = std::vector<std::size_t>{};
    indices.reserve(sizeof...(Names));
    const auto add = [&](util::symbol name) {
        auto index = std::size_t{};
        while (index != schema.field_count() && schema.field(index).name != name)
            ++index;
        if (index == schema.field_count())
            throw std::invalid_argument{"computed frame field name is absent from its resolved schema"};
        if (std::ranges::find(indices, index) != indices.end())
            throw std::invalid_argument{"computed frame field name is duplicated"};
        indices.push_back(index);
    };
    (add(util::symbol{Names.view()}), ...);
    std::ranges::sort(indices);
    return details::canonical_frame_view_runtime_type(type, std::move(indices));
}

namespace details {

template <typename Option> struct ComputedFrameRuntime;

template <util::compile_time_string... Names> struct ComputedFrameRuntime<computed_option<Names...>> {
    template <template <template <typename, auto...> typename> typename Schema>
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return frame_computed_time_series_runtime_type<Schema>(type, computed_option<Names...>{});
    }
};

} // namespace details

} // namespace bg::frame

namespace bg {

template <template <template <typename, auto...> typename> typename Schema, auto... SeriesProperties,
          auto... Properties>
class In<frame::TSF<Schema, SeriesProperties...>, Properties...>
    : public InputActivityControl<InputProperties<Properties...>::dynamic_activity> {
    using ParsedProperties = InputProperties<Properties...>;
    using ActivityControl = InputActivityControl<ParsedProperties::dynamic_activity>;

  public:
    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr)
        : ActivityControl{activity}, data_{data}, field_ordinals_{extension_descriptor().template field_ordinals<Schema>()} {}

    bool valid() const noexcept { return data_.valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }
    std::size_t size() const { return valid() ? extension_descriptor().size : 0; }
    bool contains(frame::row_id_t id) const { return valid() && extension_descriptor().rows.has(id); }
    const bg::Schema &schema() const {
        const auto input = extension_descriptor();
        if (input.schema == nullptr)
            throw std::logic_error{"frame input has no schema"};
        return *input.schema;
    }
    In<frame::TSC<SCALAR>> column(std::size_t index) const {
        const auto &resolved_schema = schema();
        if (index >= resolved_schema.field_count() ||
            index > static_cast<std::size_t>(std::numeric_limits<item_id_t>::max()))
            throw std::out_of_range{"frame column index"};
        const auto item = data_.runtime_type().item(data_.data(), static_cast<item_id_t>(index));
        const auto child_modified = data_.modified() && item.last_updated.valid() &&
                                    item.last_updated.time() == data_.last_updated();
        return In<frame::TSC<SCALAR>>{
            TimeSeriesInputData{item.runtime_type, item.last_updated, item.value, child_modified}};
    }
    In<frame::TSC<SCALAR>> column(util::symbol name) const {
        return column(extension_descriptor().field_index(name));
    }
    In<frame::TSC<SCALAR>> column(std::string_view name) const { return column(util::symbol{name}); }
    template <std::size_t Index>
        requires(Index < frame::details::frame_schema_reflection<Schema>::field_count &&
                 frame::cell_type<frame::details::frame_schema_cell_t<Index, Schema>>)
    auto cell(frame::row_id_t id) const {
        using Cell = frame::details::frame_schema_cell_t<Index, Schema>;
        auto input = extension_descriptor();
        auto column = std::optional<frame::ColumnSeriesView<Cell>>{};
        const auto binding = input.column(field_ordinals_[Index]);
        if (binding.cell_type != std::addressof(cpp_scalar_type<Cell>()) || binding.bind_column_series_view == nullptr)
            throw std::invalid_argument{"frame column type does not match the requested cell type"};
        binding.bind_column_series_view(binding.payload, std::addressof(column));
        return column->cell(id);
    }
    const void *row_domain() const { return valid() ? extension_descriptor().rows.identity : nullptr; }
    Scalar<frame::Frame<Schema>> value_view() const {
        const auto &series_runtime = static_cast<const frame::FrameSeriesRuntimeType &>(data_.runtime_type());
        return Scalar<frame::Frame<Schema>>::from(series_runtime.scalar_runtime_type(), data_.data());
    }
    template <typename Function> void for_each_row(Function &&function) const {
        if (!valid())
            return;
        auto position = std::size_t{};
        extension_descriptor().rows.each([&](frame::row_id_t id) { std::invoke(function, id, position++); });
    }
    template <typename Function> void for_each_changed(Function &&function) const {
        const auto input = extension_descriptor();
        if (!changes_current(input) || input.changed == nullptr)
            return;
        for (auto id : *input.changed)
            std::invoke(function, id);
    }
    template <typename Function> void for_each_removed(Function &&function) const {
        const auto input = extension_descriptor();
        if (!changes_current(input) || input.removed == nullptr)
            return;
        for (auto id : *input.removed)
            std::invoke(function, id);
    }
    template <std::size_t Index, typename Function>
        requires(Index < frame::details::frame_schema_reflection<Schema>::field_count)
    void for_each_modified(Function &&function) const {
        const auto input = extension_descriptor();
        const auto *modified_rows = changes_current(input)
                                        ? input.modified_at(field_ordinals_[Index])
                                        : nullptr;
        if (modified_rows == nullptr)
            return;
        for (auto id : *modified_rows)
            std::invoke(function, id);
    }
    frame::FrameReadStorage extension_descriptor() const noexcept {
        auto result = static_cast<const frame::FrameSeriesRuntimeType &>(data_.runtime_type())
                          .input_storage(data_.data());
        result.valid = valid();
        result.modified = modified();
        result.last_updated = last_updated();
        return result;
    }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }

    static constexpr auto name = ParsedProperties::name;
    static constexpr auto activity = ParsedProperties::activity;
    static constexpr auto validity = ParsedProperties::validity;
    static constexpr auto dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr auto dynamic_activity = ParsedProperties::dynamic_activity;

    using time_series_type = frame::TSF<Schema, SeriesProperties...>;

  private:
    bool changes_current(const frame::FrameReadStorage &input) const {
        return modified() && input.change_time == last_updated();
    }
    TimeSeriesInputData data_;
    std::array<std::size_t, frame::details::frame_schema_reflection<Schema>::field_count> field_ordinals_;
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
class Out<frame::TSF<Schema, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    void apply(const literal::TimeSeriesLiteral &literal) const {
        frame::details::apply_series_literal(literal, binding_.get());
    }

    const bg::Schema &schema() const {
        return static_cast<const bg::Schema &>(
            static_cast<const frame::FrameSeriesType &>(binding_->runtime_type().type()).schema_type());
    }

    frame::row_id_t add(const frame::Row<Schema> &row) const
        requires(!bg::details::schema_variable_template<Schema> &&
                 frame::details::frame_schema_declaration_traits<Schema>::fields_supported)
    {
        const auto id = [&] {
            if constexpr (frame::details::frame_schema_declaration_traits<Schema>::has_generic_fields)
                return add_typed_row<Schema>(row);
            else
                return storage().add(row, binding_->context().evaluation_time(), binding_->allocator());
        }();
        binding_->publish();
        return id;
    }

    frame::row_id_t add(std::span<const std::optional<Scalar<>>> row) const {
        const auto &fields = schema();
        if (row.size() != fields.field_count())
            throw std::invalid_argument{"frame row field count does not match its schema"};

        auto cell_values = std::vector<CellValue>(row.size());
        auto values = std::vector<const void *>(row.size());
        for (auto index = std::size_t{}; index != row.size(); ++index) {
            values[index] = decode_cell(row[index], *fields.field(index).type, cell_values[index]);
        }

        const auto id = native_storage().add(values, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
        return id;
    }

    template <template <template <typename, auto...> typename> typename SourceSchema>
        requires(!bg::details::schema_variable_template<SourceSchema> &&
                 frame::details::frame_schema_declaration_traits<SourceSchema>::fields_supported)
    frame::row_id_t add(const frame::Row<SourceSchema> &row) const {
        const auto id = add_typed_row<SourceSchema>(row);
        binding_->publish();
        return id;
    }

    template <template <template <typename, auto...> typename> typename SourceSchema>
        requires(!bg::details::schema_variable_template<SourceSchema> &&
                 frame::details::frame_schema_declaration_traits<SourceSchema>::fields_supported)
    frame::row_id_t add(const frame::RowPatch<SourceSchema> &row) const {
        const auto id = [&] {
            if constexpr (!frame::details::frame_schema_declaration_traits<SourceSchema>::has_generic_fields &&
                          std::is_same_v<frame::RowPatch<SourceSchema>, frame::RowPatch<Schema>>) {
                const auto fields = frame::details::frame_schema_reflection<SourceSchema>::fields(row);
                constexpr auto field_count = frame::details::frame_schema_reflection<SourceSchema>::field_count;
                const auto values = row_value_pointers(fields, std::make_index_sequence<field_count>{});
                return native_storage().add(values, binding_->context().evaluation_time(), binding_->allocator());
            } else {
                return add_typed_row<SourceSchema>(row);
            }
        }();
        binding_->publish();
        return id;
    }

    void set(frame::row_id_t id, const frame::RowPatch<Schema> &patch) const
        requires(!bg::details::schema_variable_template<Schema> &&
                 frame::details::frame_schema_declaration_traits<Schema>::fields_supported)
    {
        const auto changed = [&] {
            if constexpr (frame::details::frame_schema_declaration_traits<Schema>::has_generic_fields)
                return set_typed_patch<Schema>(id, patch);
            else
                return storage().set(id, patch, binding_->context().evaluation_time(), binding_->allocator());
        }();
        if (changed)
            binding_->publish();
    }

    void set(frame::row_id_t id,
             std::span<const std::pair<util::symbol, std::optional<Scalar<>>>> patch) const {
        const auto &fields = schema();
        auto cell_values = std::vector<CellValue>(fields.field_count());
        auto patches = std::vector<frame::details::NativeFrameCellPatch>(fields.field_count());
        for (const auto &[name, value] : patch) {
            const auto index = schema_field_index(fields, name);
            if (patches[index].changed)
                throw std::invalid_argument{"frame patch contains a duplicate field name"};
            patches[index] = {true, decode_cell(value, *fields.field(index).type, cell_values[index])};
        }

        if (native_storage().set(id, patches, binding_->context().evaluation_time(), binding_->allocator()))
            binding_->publish();
    }

    template <template <template <typename, auto...> typename> typename SourceSchema>
        requires(!bg::details::schema_variable_template<SourceSchema> &&
                 frame::details::frame_schema_declaration_traits<SourceSchema>::fields_supported)
    void set(frame::row_id_t id, const frame::RowPatch<SourceSchema> &patch) const {
        if (set_typed_patch<SourceSchema>(id, patch))
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
    using CellValue = std::variant<std::optional<i64>, std::optional<double>>;

    static const void *decode_cell(const std::optional<Scalar<>> &source, const Type &cell_type,
                                   CellValue &destination) {
        if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<i64>())) {
            auto &value = destination.emplace<0>();
            if (source) {
                if (std::addressof(source->type()) != std::addressof(cpp_scalar_type<i64>()))
                    throw std::invalid_argument{"frame cell type does not match its schema"};
                const auto typed_value = Scalar<i64>::from(*source);
                value = typed_value.value();
            }
            return std::addressof(value);
        }
        if (std::addressof(cell_type) == std::addressof(cpp_scalar_type<double>())) {
            auto &value = destination.emplace<1>();
            if (source) {
                if (std::addressof(source->type()) != std::addressof(cpp_scalar_type<double>()))
                    throw std::invalid_argument{"frame cell type does not match its schema"};
                const auto typed_value = Scalar<double>::from(*source);
                value = typed_value.value();
            }
            return std::addressof(value);
        }
        throw std::invalid_argument{"frame schema has an unsupported cell type"};
    }

    template <template <template <typename, auto...> typename> typename SourceSchema>
    auto typed_schema_mapping() const {
        constexpr auto field_count = frame::details::frame_schema_reflection<SourceSchema>::field_count;
        const auto &source_schema = static_cast<const bg::Schema &>(frame::frame_schema_type<SourceSchema>());
        const auto &target_schema = schema();
        if (source_schema.field_count() != field_count || source_schema.field_count() != target_schema.field_count())
            throw std::invalid_argument{"typed frame row schema does not match the output schema"};

        auto target_indices = std::array<std::size_t, field_count>{};
        auto type_map = TypeMap{};
        for (std::size_t index = 0; index != source_schema.field_count(); ++index) {
            const auto &source_field = source_schema.field(index);
            const auto target_index = schema_field_index(target_schema, source_field.name);
            const auto &target_field = target_schema.field(target_index);
            auto matches = false;
            if constexpr (frame::details::frame_schema_declaration_traits<SourceSchema>::has_generic_fields)
                matches = source_field.type->match(*target_field.type, type_map);
            else
                matches = source_field.type == target_field.type;
            if (!matches)
                throw std::invalid_argument{"typed frame row schema does not match the output schema"};
            target_indices[index] = target_index;
        }
        return target_indices;
    }

    template <template <template <typename, auto...> typename> typename SourceSchema, std::size_t Index,
              typename Fields, std::size_t FieldCount>
    const void *typed_cell_value(const Fields &fields, const std::array<std::size_t, FieldCount> &target_indices,
                                 std::array<CellValue, FieldCount> &cell_values) const {
        using SourceCell = frame::details::frame_schema_cell_t<Index, SourceSchema>;
        const auto &source = std::get<Index>(fields).value();
        if constexpr (bg::details::scalar_variable<SourceCell>) {
            const auto target = target_indices[Index];
            return decode_cell(source, *schema().field(target).type, cell_values[Index]);
        } else {
            return std::addressof(source);
        }
    }

    template <template <template <typename, auto...> typename> typename SourceSchema, typename Fields,
              std::size_t FieldCount, std::size_t... Index>
    auto typed_row_values(const Fields &fields, const std::array<std::size_t, FieldCount> &target_indices,
                          std::array<CellValue, FieldCount> &cell_values,
                          std::index_sequence<Index...>) const {
        return std::array<const void *, sizeof...(Index)>{
            typed_cell_value<SourceSchema, Index>(fields, target_indices, cell_values)...};
    }

    template <template <template <typename, auto...> typename> typename SourceSchema, typename RowType>
    frame::row_id_t add_typed_row(const RowType &row) const {
        constexpr auto field_count = frame::details::frame_schema_reflection<SourceSchema>::field_count;
        const auto fields = frame::details::frame_schema_reflection<SourceSchema>::fields(row);
        if constexpr (frame::details::frame_schema_declaration_traits<SourceSchema>::has_generic_fields) {
            const auto target_indices = typed_schema_mapping<SourceSchema>();
            auto cell_values = std::array<CellValue, field_count>{};
            const auto source_values = typed_row_values<SourceSchema>(
                fields, target_indices, cell_values, std::make_index_sequence<field_count>{});
            auto values = std::array<const void *, field_count>{};
            for (auto index = std::size_t{}; index != field_count; ++index)
                values[target_indices[index]] = source_values[index];
            return native_storage().add(values, binding_->context().evaluation_time(), binding_->allocator());
        } else {
            const auto target_indices = typed_schema_mapping<SourceSchema>();
            const auto source_values = row_value_pointers(fields, std::make_index_sequence<field_count>{});
            auto values = std::array<const void *, field_count>{};
            for (auto index = std::size_t{}; index != field_count; ++index)
                values[target_indices[index]] = source_values[index];
            return native_storage().add(values, binding_->context().evaluation_time(), binding_->allocator());
        }
    }

    template <template <template <typename, auto...> typename> typename SourceSchema, std::size_t Index,
              typename Fields, std::size_t FieldCount>
    frame::details::NativeFrameCellPatch typed_patch_value(
        const Fields &fields, const std::array<std::size_t, FieldCount> &target_indices,
        std::array<CellValue, FieldCount> &cell_values) const {
        const auto &source = std::get<Index>(fields);
        if (!source.changed())
            return {};
        return {true, typed_cell_value<SourceSchema, Index>(fields, target_indices, cell_values)};
    }

    template <template <template <typename, auto...> typename> typename SourceSchema, typename Fields,
              std::size_t FieldCount, std::size_t... Index>
    auto typed_patch_values(const Fields &fields, const std::array<std::size_t, FieldCount> &target_indices,
                            std::array<CellValue, FieldCount> &cell_values,
                            std::index_sequence<Index...>) const {
        return std::array<frame::details::NativeFrameCellPatch, sizeof...(Index)>{
            typed_patch_value<SourceSchema, Index>(fields, target_indices, cell_values)...};
    }

    template <template <template <typename, auto...> typename> typename SourceSchema, typename RowPatchType>
    bool set_typed_patch(frame::row_id_t id, const RowPatchType &patch) const {
        constexpr auto field_count = frame::details::frame_schema_reflection<SourceSchema>::field_count;
        const auto target_indices = typed_schema_mapping<SourceSchema>();
        const auto fields = frame::details::frame_schema_reflection<SourceSchema>::fields(patch);
        if constexpr (frame::details::frame_schema_declaration_traits<SourceSchema>::has_generic_fields) {
            auto cell_values = std::array<CellValue, field_count>{};
            const auto source_patches = typed_patch_values<SourceSchema>(
                fields, target_indices, cell_values, std::make_index_sequence<field_count>{});
            auto patches = std::array<frame::details::NativeFrameCellPatch, field_count>{};
            for (auto index = std::size_t{}; index != field_count; ++index)
                patches[target_indices[index]] = source_patches[index];
            return native_storage().set(id, patches, binding_->context().evaluation_time(), binding_->allocator());
        } else {
            const auto source_patches = row_patch_pointers(fields, std::make_index_sequence<field_count>{});
            auto patches = std::array<frame::details::NativeFrameCellPatch, field_count>{};
            for (auto index = std::size_t{}; index != field_count; ++index)
                patches[target_indices[index]] = source_patches[index];
            return native_storage().set(id, patches, binding_->context().evaluation_time(), binding_->allocator());
        }
    }

    template <typename Fields, std::size_t... Index>
    static auto row_value_pointers(const Fields &fields, std::index_sequence<Index...>) {
        return std::array<const void *, sizeof...(Index)>{
            static_cast<const void *>(std::addressof(std::get<Index>(fields).value()))...};
    }

    template <typename Fields, std::size_t... Index>
    static auto row_patch_pointers(const Fields &fields, std::index_sequence<Index...>) {
        return std::array<frame::details::NativeFrameCellPatch, sizeof...(Index)>{
            frame::details::NativeFrameCellPatch{std::get<Index>(fields).changed(),
                                                  std::addressof(std::get<Index>(fields).value())}...};
    }

    static std::size_t schema_field_index(const bg::Schema &fields, util::symbol name) {
        for (auto index = std::size_t{}; index != fields.field_count(); ++index)
            if (fields.field(index).name == name)
                return index;
        throw std::out_of_range{"unknown frame field name"};
    }

    frame::NativeFrame<Schema> storage() const noexcept {
        return frame::NativeFrame<Schema>{binding_->mutable_data()};
    }
    frame::NativeFrameStorage &native_storage() const noexcept {
        return *static_cast<frame::NativeFrameStorage *>(binding_->mutable_data());
    }
    TimeSeriesOutputBinding binding_;
};

namespace frame::details {

class FrameViewOutputImplementation {
  public:
    explicit FrameViewOutputImplementation(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    const bg::Schema &schema() const {
        return static_cast<const bg::Schema &>(
            static_cast<const ::bg::frame::FrameSeriesType &>(binding_->runtime_type().type()).schema_type());
    }

    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

  protected:
    template <template <template <typename, auto...> typename> typename SourceSchema,
              auto... SeriesProperties, auto... InputProperties>
    bool bind_source(const In<::bg::frame::TSF<SourceSchema, SeriesProperties...>, InputProperties...> &source) const {
        const auto input = source.extension_descriptor();
        auto &output = runtime_storage();
        if (input.schema == nullptr)
            throw std::logic_error{"TSF view has no resolved source schema"};
        if (!input.valid) {
            if (output.valid)
                binding_->invalidate();
            return false;
        }
        const auto changed = output.bind(input, {}, std::nullopt, nullptr, nullptr,
                                         binding_->context().evaluation_time(), binding_->allocator());
        if (changed) {
            for (auto index : output.changed_columns)
                binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
        return changed;
    }

    template <util::compile_time_string Name, ::bg::frame::cell_type Cell>
    void set_computed_cell(::bg::frame::row_id_t id, std::optional<Cell> value) const {
        auto &output = runtime_storage();
        const auto index = output.field_index(util::symbol{Name.view()});
        if (!output.is_computed(index))
            throw std::invalid_argument{"frame output field is not declared computed"};
        if (output.layout->schema->field(index).type != std::addressof(cpp_scalar_type<Cell>()))
            throw std::invalid_argument{"computed frame cell type does not match its schema"};
        if (output.set_computed(index, id, std::addressof(value), binding_->context().evaluation_time(),
                                binding_->allocator())) {
            binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
    }

    template <util::compile_time_string Name>
    void set_computed_null(::bg::frame::row_id_t id) const {
        auto &output = runtime_storage();
        const auto index = output.field_index(util::symbol{Name.view()});
        if (!output.is_computed(index))
            throw std::invalid_argument{"frame output field is not declared computed"};
        auto value = std::variant<std::optional<i64>, std::optional<double>>{};
        const void *cell = nullptr;
        if (output.layout->schema->field(index).type == std::addressof(cpp_scalar_type<i64>()))
            cell = std::addressof(value.emplace<0>());
        else if (output.layout->schema->field(index).type == std::addressof(cpp_scalar_type<double>()))
            cell = std::addressof(value.emplace<1>());
        else
            throw std::invalid_argument{"computed frame schema has an unsupported cell type"};
        if (output.set_computed(index, id, cell, binding_->context().evaluation_time(), binding_->allocator())) {
            binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
    }

    template <util::compile_time_string Name, ::bg::frame::cell_type Cell>
    void set_computed_batch(std::span<const ::bg::frame::row_id_t> ids,
                            const ::bg::frame::ColumnBatch<Cell> &batch) const {
        if (batch.size != ids.size())
            throw std::invalid_argument{"computed frame batch length does not match its row IDs"};
        auto &output = runtime_storage();
        const auto index = output.field_index(util::symbol{Name.view()});
        if (!output.is_computed(index))
            throw std::invalid_argument{"frame output field is not declared computed"};
        if (output.layout->schema->field(index).type != std::addressof(cpp_scalar_type<Cell>()))
            throw std::invalid_argument{"computed frame batch type does not match its schema"};
        auto has_valid_cell = false;
        for (std::size_t position = 0; position != ids.size(); ++position) {
            if (!output.source_rows.has(ids[position]))
                throw std::out_of_range{"computed frame batch row ID"};
            has_valid_cell = has_valid_cell || batch.is_valid(position);
        }
        if (has_valid_cell && batch.values == nullptr)
            throw std::invalid_argument{"computed frame batch has valid cells but no values"};
        if (ids.empty())
            return;

        const auto time = binding_->context().evaluation_time();
        output.begin_cycle(time);
        auto changed = false;
        for (std::size_t position = 0; position != ids.size(); ++position) {
            auto value = std::optional<Cell>{};
            if (batch.is_valid(position))
                value = batch.values[position];
            changed = output.write_computed(index, ids[position], std::addressof(value),
                                            binding_->allocator()) || changed;
        }
        output.finish_computed_updates(index, time, changed);
        if (changed) {
            binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
    }

    ::bg::frame::RuntimeFrameViewOutputStorage &runtime_storage() const noexcept {
        return *static_cast<::bg::frame::RuntimeFrameViewOutputStorage *>(binding_->mutable_data());
    }

    TimeSeriesOutputBinding binding_;
};

} // namespace frame::details

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
class Out<frame::TSF<Schema, Properties...>, frame::view>
    : private frame::details::FrameViewOutputImplementation {
    using Base = frame::details::FrameViewOutputImplementation;

  public:
    using Base::Base;
    using Base::invalidate;
    using Base::runtime_type;
    using Base::schema;
    using Base::valid;

    template <template <template <typename, auto...> typename> typename SourceSchema,
              auto... SeriesProperties, auto... InputProperties>
    bool bind_projection(const In<frame::TSF<SourceSchema, SeriesProperties...>, InputProperties...> &source,
                         std::span<const std::size_t> source_indices) const {
        const auto input = source.extension_descriptor();
        auto &output = runtime_storage();
        if (input.schema == nullptr)
            throw std::logic_error{"TSF view has no resolved source schema"};
        if (source_indices.size() != output.layout->fields.size())
            throw std::invalid_argument{"frame view projection column count does not match its output schema"};
        for (auto index = std::size_t{}; index != source_indices.size(); ++index) {
            const auto source_index = source_indices[index];
            if (source_index >= input.schema->field_count())
                throw std::out_of_range{"frame view projection source column index"};
            if (input.schema->field(source_index).type != output.layout->schema->field(index).type)
                throw std::invalid_argument{"frame view projection column type does not match its output schema"};
        }
        if (!input.valid) {
            if (output.valid)
                binding_->invalidate();
            return false;
        }
        const auto changed = output.bind(input, source_indices, std::nullopt, nullptr, nullptr,
                                         binding_->context().evaluation_time(), binding_->allocator());
        if (changed) {
            for (auto index : output.changed_columns)
                binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
        return changed;
    }

    template <template <template <typename, auto...> typename> typename SourceSchema,
              auto... SeriesProperties, auto... InputProperties, util::compile_time_string... Names>
    bool bind_projection(
        const In<frame::TSF<SourceSchema, SeriesProperties...>, InputProperties...> &source,
        frame::ColumnNames<Names...>) const {
        const auto input = source.extension_descriptor();
        const auto source_indices = std::array<std::size_t, sizeof...(Names)>{
            input.field_index(util::symbol{Names.view()})...};
        return bind_projection(source, std::span<const std::size_t>{source_indices});
    }

    template <template <template <typename, auto...> typename> typename SourceSchema,
              auto... SeriesProperties, auto... InputProperties>
    bool bind_projection(
        const In<frame::TSF<SourceSchema, SeriesProperties...>, InputProperties...> &source,
        std::span<const util::symbol> source_names) const {
        const auto input = source.extension_descriptor();
        if (source_names.size() != schema().field_count())
            throw std::invalid_argument{"frame view projection column count does not match its output schema"};
        auto source_indices = std::vector<std::size_t>{};
        source_indices.reserve(source_names.size());
        for (const auto name : source_names)
            source_indices.push_back(input.field_index(name));
        return bind_projection(source, std::span<const std::size_t>{source_indices});
    }

    template <util::compile_time_string Name,
              template <template <typename, auto...> typename> typename SourceSchema,
              auto... SeriesProperties, auto... InputProperties, typename Predicate>
    bool bind_filter(const In<frame::TSF<SourceSchema, SeriesProperties...>, InputProperties...> &source,
                     Predicate &&predicate) const {
        const auto input = source.extension_descriptor();
        auto &output = runtime_storage();
        if (!input.valid) {
            if (output.valid)
                binding_->invalidate();
            return false;
        }
        using PredicateType = std::remove_reference_t<Predicate>;
        auto *predicate_context = std::addressof(predicate);
        const auto predicate_function = [](void *context, frame::row_id_t id) {
            return std::invoke(*static_cast<PredicateType *>(context), id);
        };
        const auto predicate_index = output.field_index(util::symbol{Name.view()});
        const auto changed = output.bind(input, {}, predicate_index, predicate_context,
                                         predicate_function, binding_->context().evaluation_time(),
                                         binding_->allocator());
        if (changed) {
            for (auto index : output.changed_columns)
                binding_->publish_item(binding_->runtime_type(), static_cast<item_id_t>(index));
            binding_->publish();
        }
        return changed;
    }

    using time_series_type = frame::TSF<Schema, Properties...>;

  private:
    using Base::binding_;
    using Base::runtime_storage;
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties, auto Option>
    requires frame::is_computed_option<std::remove_cv_t<decltype(Option)>>::value
class Out<frame::TSF<Schema, Properties...>, Option>
    : private frame::details::FrameViewOutputImplementation {
    using Base = frame::details::FrameViewOutputImplementation;
    using OptionType = std::remove_cv_t<decltype(Option)>;

  public:
    static_assert(OptionType::field_count != 0, "computed TSF output requires at least one field name");
    using Base::Base;
    using Base::invalidate;
    using Base::runtime_type;
    using Base::schema;
    using Base::valid;
    using time_series_type = frame::TSF<Schema, Properties...>;

    template <template <template <typename, auto...> typename> typename SourceSchema,
              auto... SeriesProperties, auto... InputProperties>
    bool bind(const In<frame::TSF<SourceSchema, SeriesProperties...>, InputProperties...> &source) const {
        return this->bind_source(source);
    }

    template <util::compile_time_string Name, frame::cell_type Cell>
    void set(frame::row_id_t id, std::optional<Cell> value) const {
        this->template set_computed_cell<Name>(id, std::move(value));
    }

    template <util::compile_time_string Name, frame::cell_type Cell>
    void set(frame::row_id_t id, Cell value) const {
        this->template set_computed_cell<Name>(id, std::optional<Cell>{std::move(value)});
    }

    template <util::compile_time_string Name>
    void set(frame::row_id_t id, std::nullopt_t) const {
        this->template set_computed_null<Name>(id);
    }

    template <util::compile_time_string Name, frame::cell_type Cell>
    void set_batch(std::span<const frame::row_id_t> ids, const frame::ColumnBatch<Cell> &batch) const {
        this->template set_computed_batch<Name>(ids, batch);
    }
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct time_series_type_traits<frame::TSF<Schema, Properties...>> {
    using semantic_type = TimeSeriesType;
    static constexpr bool generic = frame::details::frame_schema_declaration_traits<Schema>::is_schema_variable ||
                                    frame::details::frame_schema_declaration_traits<Schema>::has_generic_fields;
    static constexpr bool supported = frame::details::frame_schema_declaration_traits<Schema>::fields_supported;

    template <typename Scope, std::size_t Index, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if (!guarantee.is_resolved()) {
            return frame::frame_series_type(frame::frame_schema_type<Schema>(), guarantee);
        }
        return frame::frame_time_series_type<Schema>(*guarantee.value());
    }
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_declaration_traits<frame::Frame<Schema>> {
    static constexpr bool generic = frame::details::frame_schema_declaration_traits<Schema>::is_schema_variable ||
                                    frame::details::frame_schema_declaration_traits<Schema>::has_generic_fields;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = frame::details::frame_schema_declaration_traits<Schema>::fields_supported;
};

template <template <template <typename, auto...> typename> typename Schema>
struct declaration_variable_traits<frame::Frame<Schema>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        frame::details::frame_schema_declaration_traits<Schema>::for_each(std::forward<Visitor>(visitor));
    }
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct declaration_variable_traits<frame::TSF<Schema, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        frame::details::frame_schema_declaration_traits<Schema>::for_each(visitor);
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

template <template <template <typename, auto...> typename> typename Schema, auto... Properties, auto Option>
    requires frame::is_computed_option<std::remove_cv_t<decltype(Option)>>::value
struct output_declaration_traits<Out<frame::TSF<Schema, Properties...>, Option>> {
    using time_series_type = frame::TSF<Schema, Properties...>;
    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return frame::details::ComputedFrameRuntime<std::remove_cv_t<decltype(Option)>>::template get<Schema>(type);
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
    static_assert(std::is_arithmetic_v<Threshold>, "filter threshold must be numeric");

    void operator()(In<TSF<Schema>, optional> input, Threshold threshold, Out<TSF<Schema>, frame::view> output) const {
        if constexpr (bg::details::schema_variable_template<Schema> ||
                      frame_schema_declaration_traits<Schema>::has_generic_fields) {
            if (!input.valid()) {
                output.template bind_filter<Name>(input, [](row_id_t) { return false; });
                return;
            }
            const auto column = input.column(util::symbol{Name.view()});
            const auto &column_type = static_cast<const ColumnSeriesType &>(column.runtime_type().type());
            if (std::addressof(column_type.cell_type()) == std::addressof(cpp_scalar_type<i64>())) {
                const auto typed_column = column.template input<i64>();
                output.template bind_filter<Name>(input, [&](row_id_t id) {
                    const auto value = typed_column.cell(id);
                    return value && *value > threshold;
                });
                return;
            }
            if (std::addressof(column_type.cell_type()) == std::addressof(cpp_scalar_type<double>())) {
                const auto typed_column = column.template input<double>();
                output.template bind_filter<Name>(input, [&](row_id_t id) {
                    const auto value = typed_column.cell(id);
                    return value && *value > threshold;
                });
                return;
            }
            throw std::invalid_argument{"filter predicate column has an unsupported cell type"};
        } else {
            constexpr auto predicate_index = frame_column_index<Name, Schema>();
            static_assert(predicate_index < bg::details::hierarchical_scalar_schema_reflection<Schema>::field_count,
                          "filter names an unknown predicate column");
            using Cell = frame_schema_cell_t<predicate_index, Schema>;
            static_assert(cell_type<Cell>, "filter predicate column must have a supported numeric cell type");
            output.template bind_filter<Name>(input, [&](row_id_t id) {
                const auto value = input.template cell<predicate_index>(id);
                return value && *value > threshold;
            });
        }
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
        const auto &type = frame::frame_type(frame::frame_schema_type<Schema>());
        if (type.is_resolved())
            frame::frame_snapshot_runtime_type(type);
        return type;
    }
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_storage_traits<frame::Frame<Schema>> {
    using stored_type = frame::FrameSnapshotStorage;
    using value_type = frame::FrameView<Schema>;
    static constexpr bool supports_byte_copy_initialization = true;
    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = false;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = true;
    static constexpr bool assign_uses_memory_resource = true;

    static const ScalarRuntimeType &runtime_type(const ScalarType &type) {
        return frame::frame_snapshot_runtime_type(static_cast<const frame::FrameType &>(type));
    }

    static value_type read(const stored_type &source) {
        const auto input = cpp_scalar_runtime_type<frame::Frame<Schema>>().borrow(std::addressof(source));
        return read_value(input);
    }

    static value_type read_value(const ScalarData &source) {
        const auto &runtime = static_cast<const frame::FrameScalarRuntimeType &>(source.runtime_type());
        return frame::details::frame_view<Schema>(runtime.read_storage(source), source);
    }

    static ScalarData borrow_value(const value_type &value) noexcept {
        if (!value.source_data().empty())
            return value.source_data();
        return frame::details::frame_view_scalar_runtime_type<Schema>().borrow(std::addressof(value));
    }

    static bool equals(value_type left, value_type right) { return borrow_value(left).equals(borrow_value(right)); }

    static const ScalarRuntimeType &runtime_type_for_copy(const ScalarData &source) {
        const auto &runtime = static_cast<const frame::FrameScalarRuntimeType &>(source.runtime_type());
        const auto storage = runtime.read_storage(source);
        if (storage.schema == nullptr)
            throw std::invalid_argument{"frame scalar source has no schema"};
        return frame::frame_snapshot_runtime_type(frame::frame_type(*storage.schema));
    }

    static void construct(stored_type *destination, value_type source, allocator &memory) {
        std::construct_at(destination);
        try {
            copy_value(*destination, borrow_value(source), memory);
        } catch (...) {
            if (auto *owner = std::exchange(destination->owner, nullptr))
                owner->release();
            std::destroy_at(destination);
            throw;
        }
    }

    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory) {
        static_cast<void>(memory);
        auto *owner = source.owner;
        if (owner != nullptr)
            owner->retain();
        std::construct_at(destination, stored_type{owner});
    }

    static void assign(stored_type &destination, value_type source, allocator &memory) {
        auto replacement = stored_type{};
        construct(std::addressof(replacement), std::move(source), memory);
        auto *previous = std::exchange(destination.owner, std::exchange(replacement.owner, nullptr));
        if (previous != nullptr)
            previous->release();
        std::destroy_at(std::addressof(replacement));
    }

    static void destroy(stored_type *value, allocator &) noexcept {
        if (auto *owner = std::exchange(value->owner, nullptr))
            owner->release();
        std::destroy_at(value);
    }

  private:
    static void copy_value(stored_type &destination, const ScalarData &source, allocator &memory) {
        frame::details::copy_frame_snapshot_value(destination, source, memory);
    }
};

} // namespace bg

namespace bg::frame {

inline ScalarValue<Frame<bg::SCHEMA>> make_frame_value(allocator &memory, const bg::Schema &schema,
                                                       std::size_t row_count,
                                                       std::span<const ColumnView<>> columns) {
    if (!schema.is_resolved())
        throw std::invalid_argument{"frame value requires a resolved schema"};
    if (schema.field_count() != columns.size())
        throw std::invalid_argument{"frame column count does not match the schema"};

    for (auto index = std::size_t{}; index != columns.size(); ++index) {
        const auto &field = schema.field(index);
        if (field.optional)
            throw std::invalid_argument{"frame schema columns must be present"};
        const auto &column = columns[index];
        const auto binding = details::runtime_frame_column_binding(column);
        if (field.type != binding.cell_type)
            throw std::invalid_argument{"frame column type does not match the schema"};
        if (binding.cell_type == std::addressof(cpp_scalar_type<i64>())) {
            if (column.view<i64>().size() != row_count)
                throw std::invalid_argument{"frame columns have different lengths"};
        } else if (column.view<double>().size() != row_count) {
            throw std::invalid_argument{"frame columns have different lengths"};
        }
    }

    auto source = FrameReadStorage{};
    source.schema = std::addressof(schema);
    source.valid = true;
    source.size = row_count;
    source.columns_context = std::addressof(columns);
    source.column_binding = [](const void *context, std::size_t index) {
        const auto &values = *static_cast<const std::span<const ColumnView<> > *>(context);
        return details::runtime_frame_column_binding(values[index]);
    };
    details::validate_frame_key_uniqueness(source);

    auto staged = FrameSnapshotStorage{};
    details::copy_frame_snapshot_storage(staged, details::native_frame_layout(schema), source, memory);
    auto release_staged = util::scope_exit{[&]() noexcept {
        if (auto *owner = std::exchange(staged.owner, nullptr))
            owner->release();
    }};
    const auto &runtime = frame_snapshot_runtime_type(frame_type(schema));
    const auto borrowed = Scalar<Frame<bg::SCHEMA>>::from(runtime, std::addressof(staged));
    return ScalarValue<Frame<bg::SCHEMA>>{borrowed, memory};
}

inline ScalarValue<Frame<bg::SCHEMA>> make_frame_value(const bg::Schema &schema, std::size_t row_count,
                                                       std::span<const ColumnView<>> columns) {
    auto memory = bg::details::current_scalar_allocator();
    return make_frame_value(memory, schema, row_count, columns);
}

} // namespace bg::frame
