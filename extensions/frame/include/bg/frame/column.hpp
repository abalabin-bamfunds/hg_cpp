#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/declaration.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/input_data.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/util/compile_time_string.hpp>
#include <bg/util/index.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>
#include <bg/util/byte_copy.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bg::frame {
struct view_option {};
inline constexpr view_option view{};
template <util::compile_time_string... Names> struct computed_option {
    static constexpr auto field_count = sizeof...(Names);
};
template <util::compile_time_string... Names>
inline constexpr computed_option<Names...> computed{};
template <typename T> struct is_computed_option : std::false_type {};
template <util::compile_time_string... Names>
struct is_computed_option<computed_option<Names...>> : std::true_type {};

using row_id_t = std::uint32_t;
inline constexpr auto no_row = std::numeric_limits<row_id_t>::max();
template <typename T>
concept cell_type = std::same_as<T, bool> || std::same_as<T, i64> || std::same_as<T, double> ||
                    std::same_as<T, string> || std::same_as<T, symbol>;
template <typename T>
concept numeric_cell_type = std::same_as<T, i64> || std::same_as<T, double>;
template <typename T> using cell_value_t = typename scalar_storage_traits<T>::value_type;
template <typename T> using cell_stored_t = typename scalar_storage_traits<T>::stored_type;
inline constexpr auto column_cell_storage_size = std::max(sizeof(i64), sizeof(double));

template <typename Function> decltype(auto) visit_cell_type(const Type &type, Function &&function) {
    using result_type = std::invoke_result_t<Function, std::type_identity<bool>>;
    const auto *builtin = type_cast<BuiltinScalarType>(&type);
    if (builtin == nullptr)
        throw std::invalid_argument{"unsupported frame cell type"};
    return builtin->visit_builtin_type([&]<typename T>() -> result_type {
        if constexpr (cell_type<T>)
            return std::invoke(std::forward<Function>(function), std::type_identity<T>{});
        else
            throw std::invalid_argument{"unsupported frame cell type"};
    });
}

template <cell_type T> inline constexpr bool packed_column_value = std::same_as<T, bool>;

template <cell_type T> struct ColumnValueTraits {
    using pointer = const cell_stored_t<T> *;
    using value_type = cell_value_t<T>;

    static value_type read(pointer values, std::size_t position, std::uint64_t) noexcept {
        return scalar_storage_traits<T>::read(values[position]);
    }
    static pointer subspan(pointer values, std::size_t offset, std::uint64_t &) noexcept {
        return values == nullptr ? nullptr : values + offset;
    }
};

// Reads source-backed batch cells. `first_position` is advanced when a batch is subspanned.
template <cell_type T> struct ColumnBatchSource {
    using value_type = cell_value_t<T>;

    const void *context{};
    value_type (*read_value)(const void *, std::size_t) noexcept{};
    std::size_t first_position{};
};
template <cell_type T>
inline constexpr bool source_backed_column_type = std::same_as<T, string> || std::same_as<T, symbol>;
template <cell_type T>
using column_batch_source_t = std::conditional_t<source_backed_column_type<T>, ColumnBatchSource<T>, std::monostate>;

template <> struct ColumnValueTraits<bool> {
    using pointer = const std::uint8_t *;

    static bool read(pointer values, std::size_t position, std::uint64_t bit_offset) noexcept {
        const auto bit = bit_offset + position;
        return (values[bit / 8] & (std::uint8_t{1} << (bit % 8))) != 0;
    }
    static pointer subspan(pointer values, std::size_t offset, std::uint64_t &bit_offset) noexcept {
        const auto total = bit_offset + offset;
        bit_offset = total % 8;
        return values == nullptr ? nullptr : values + total / 8;
    }
};

// Extension descriptor for Column<T>; node declarations use Column<T> and do not access this type directly.
class ColumnType final : public ScalarType {
  public:
    explicit ColumnType(const ScalarType &cell_type) noexcept : cell_type_{cell_type} {}

    void format_name(std::string &result) const override {
        result += "Column<";
        cell_type().format_name(result);
        result += '>';
    }
    const ScalarType &cell_type() const noexcept { return cell_type_; }
    bool is_resolved() const noexcept override { return cell_type_.is_resolved(); }
    bool supports_equality() const noexcept override { return cell_type_.supports_equality(); }
    const Type &resolve(const TypeMap &type_map) const override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const ScalarType &cell_type_;
};

// Extension factory for semantic descriptors used during type resolution.
const ColumnType &column_type(const ScalarType &cell_type);

template <typename T> class NativeColumn;
template <typename T> class NativeColumnData;
template <typename T> class ColumnSeriesView;
template <typename T> class Column;
template <typename T> class ColumnReadSource;
class NativeRows;
class NativeRowSlots;
template <typename T> struct ColumnStorage;
// ids is valid only during the each_span callback. A non-null physical_rows proves that ids[i]
// maps to slot first_slot + i in that row store.
struct RowSetSpan {
    std::span<const row_id_t> ids{};
    const NativeRowSlots *physical_rows{};
    row_id_t first_slot{};
};
struct RowSet {
    using size_function = std::size_t (*)(const void *) noexcept;
    using contains_function = bool (*)(const void *, row_id_t) noexcept;
    using id_at_function = row_id_t (*)(const void *, std::size_t);
    using visit_function = void (*)(void *, row_id_t);
    using for_each_function = void (*)(const void *, void *, visit_function);
    using span_visit_function = void (*)(void *, const RowSetSpan &);
    using for_each_span_function = void (*)(const void *, void *, span_visit_function);

    const void *context{};
    const NativeRows *native_rows{};
    const void *identity{};
    size_function size{};
    contains_function contains{};
    id_at_function id_at{};
    for_each_function for_each{};
    for_each_span_function for_each_span{};

    // Compares descriptor fields; distinct descriptors can enumerate the same rows.
    friend bool operator==(const RowSet &, const RowSet &) = default;

    static RowSet native(const NativeRows &rows) noexcept;
    std::size_t count() const noexcept { return context == nullptr ? 0 : size(context); }
    bool has(row_id_t id) const noexcept { return context != nullptr && contains(context, id); }
    row_id_t at(std::size_t position) const { return id_at(context, position); }

    template <typename Function> void each(Function &&function) const {
        if (context == nullptr)
            return;
        auto invoke = [&](row_id_t id) { std::invoke(function, id); };
        for_each(context, std::addressof(invoke), [](void *target, row_id_t id) {
            std::invoke(*static_cast<decltype(invoke) *>(target), id);
        });
    }

    template <typename Function> void each_span(Function &&function) const {
        if (context == nullptr)
            return;
        if (for_each_span == nullptr) {
            each([&](row_id_t id) {
                const auto span = RowSetSpan{std::span<const row_id_t>{std::addressof(id), 1}};
                std::invoke(function, span);
            });
            return;
        }
        auto invoke = [&](const RowSetSpan &span) { std::invoke(function, span); };
        for_each_span(context, std::addressof(invoke), [](void *target, const RowSetSpan &span) {
            std::invoke(*static_cast<decltype(invoke) *>(target), span);
        });
    }
};

class SelectedRows {
  public:
    SelectedRows() noexcept = default;
    SelectedRows(const SelectedRows &) = delete;

    void bind(RowSet source) noexcept { source_ = source; }
    RowSet source() const noexcept { return source_; }
    bool contains(row_id_t id) const noexcept { return selected_.contains(id); }
    std::size_t size() const noexcept { return size_; }
    bool include(row_id_t id, allocator memory) {
        if (selected_.contains(id))
            return false;
        selected_.set(id, memory.memory_resource());
        ++size_;
        return true;
    }
    bool exclude(row_id_t id) noexcept {
        if (!selected_.reset(id))
            return false;
        --size_;
        return true;
    }
    template <typename Function> void for_each_selected(Function &&function) const {
        for (auto id : selected_)
            std::invoke(function, id);
    }
    void clear() noexcept {
        selected_.clear();
        size_ = 0;
    }
    void release(allocator memory) noexcept {
        selected_.release(memory.memory_resource());
        source_ = {};
        size_ = 0;
    }

  private:
    RowSet source_{};
    util::index<> selected_;
    std::size_t size_{};
};
template <typename T, auto... Properties>
struct TSC : bg::TimeSeriesDeclaration<TSC<T, Properties...>, Properties...> {};

template <cell_type T> const bg::TimeSeriesType &column_time_series_type(bg::TimeSeriesGuarantee guarantee);
template <cell_type T>
const bg::TimeSeriesRuntimeType &column_time_series_runtime_type(const bg::TimeSeriesType &type);
template <cell_type T>
const bg::TimeSeriesRuntimeType &column_view_time_series_runtime_type(const bg::TimeSeriesType &type);

template <typename T> class ColumnSnapshotValues {
  public:
    using stored_type = cell_stored_t<T>;
    using pointer = const stored_type *;

    explicit ColumnSnapshotValues(std::pmr::memory_resource *memory) : values_{memory} {}
    void resize(std::size_t size) { values_.resize(size); }
    std::size_t size() const noexcept { return values_.size(); }
    pointer data() const noexcept { return values_.data(); }
    cell_value_t<T> value_at(std::size_t position) const noexcept {
        return scalar_storage_traits<T>::read(values_[position]);
    }
    stored_type &stored_at(std::size_t position) noexcept { return values_[position]; }
    const stored_type &stored_at(std::size_t position) const noexcept { return values_[position]; }
    void copy_construct(std::size_t position, const stored_type &source, allocator &memory) {
        if constexpr (requires { scalar_storage_traits<T>::copy_construct(&values_[position], source, memory); })
            scalar_storage_traits<T>::copy_construct(std::addressof(values_[position]), source, memory);
        else
            scalar_storage_traits<T>::assign(values_[position], scalar_storage_traits<T>::read(source), memory);
    }

  private:
    std::pmr::vector<stored_type> values_;
};

template <> class ColumnSnapshotValues<bool> {
  public:
    using pointer = const std::uint8_t *;

    explicit ColumnSnapshotValues(std::pmr::memory_resource *memory) : values_{memory} {}
    void resize(std::size_t size) {
        size_ = size;
        values_.resize(size / 8 + (size % 8 != 0));
    }
    std::size_t size() const noexcept { return size_; }
    pointer data() const noexcept { return values_.data(); }
    bool value_at(std::size_t position) const noexcept {
        return (values_[position / 8] & (std::uint8_t{1} << (position % 8))) != 0;
    }
    void set_value(std::size_t position, bool value) noexcept {
        auto &byte = values_[position / 8];
        const auto mask = std::uint8_t{1} << (position % 8);
        if (value)
            byte |= mask;
        else
            byte &= static_cast<std::uint8_t>(~mask);
    }

  private:
    std::pmr::vector<std::uint8_t> values_;
    std::size_t size_{};
};

template <typename T> class ColumnSnapshot {
  public:
    explicit ColumnSnapshot(allocator memory) : memory_{memory}, values_{&memory.memory_resource()} {}
    ColumnSnapshot(const ColumnSnapshot &) = delete;
    static ColumnSnapshot *create(allocator memory, std::size_t size) {
        if (size > no_row)
            throw std::length_error{"column snapshot exceeds the supported row count"};
        auto *result = memory.new_object<ColumnSnapshot>(memory);
        try {
            result->values_.resize(size);
        } catch (...) {
            memory.delete_object(result);
            throw;
        }
        return result;
    }
    void retain() noexcept { ++references_; }
    void release() noexcept {
        if (--references_ != 0)
            return;
        auto memory = memory_;
        if constexpr (scalar_storage_traits<T>::requires_finalization)
            for (const auto position : valid_)
                scalar_storage_traits<T>::destroy(std::addressof(values_.stored_at(position)), memory);
        valid_.release(memory.memory_resource());
        memory.delete_object(this);
    }
    allocator memory() const noexcept { return memory_; }
    std::size_t size() const noexcept { return values_.size(); }
    typename ColumnSnapshotValues<T>::pointer values() const noexcept { return values_.data(); }
    cell_value_t<T> value_at(std::size_t position) const noexcept { return values_.value_at(position); }
    void set_value(std::size_t position, cell_value_t<T> value) {
        if constexpr (std::same_as<T, bool>) {
            values_.set_value(position, value);
        } else {
            scalar_storage_traits<T>::assign(values_.stored_at(position), value, memory_);
        }
    }
    bool is_valid(std::size_t position) const noexcept { return valid_.contains(static_cast<row_id_t>(position)); }
    void copy_stored_value(std::size_t position, const cell_stored_t<T> &source) {
        values_.copy_construct(position, source, memory_);
    }
    void set_valid(row_id_t position) { valid_.set(position, memory_.memory_resource()); }
    const util::index<> &valid() const noexcept { return valid_; }

  private:
    std::atomic_size_t references_{1};
    allocator memory_;
    ColumnSnapshotValues<T> values_;
    util::index<> valid_;
};

// Raw values are readable only when is_valid is true. Native null positions need not contain a T object.
class ColumnBatchValidity {
  public:
    enum class Kind : std::uint8_t { all_valid, all_invalid, index, bitmap, callback };

    ColumnBatchValidity() noexcept = default;
    static ColumnBatchValidity all_valid() noexcept { return ColumnBatchValidity{Kind::all_valid}; }
    static ColumnBatchValidity all_invalid() noexcept { return ColumnBatchValidity{Kind::all_invalid}; }
    static ColumnBatchValidity from_index(const util::index<> &positions) noexcept {
        auto result = ColumnBatchValidity{Kind::index};
        result.positions_ = std::addressof(positions);
        return result;
    }
    static ColumnBatchValidity from_bitmap(const std::uint8_t *bits, std::uint64_t offset) noexcept {
        auto result = ColumnBatchValidity{Kind::bitmap};
        result.bits_ = bits;
        result.bit_offset_ = offset;
        return result;
    }
    static ColumnBatchValidity from_callback(const void *context,
                                            bool (*contains)(const void *, std::uint64_t, std::size_t)) noexcept {
        auto result = ColumnBatchValidity{Kind::callback};
        result.context_ = context;
        result.contains_ = contains;
        return result;
    }

    bool contains(std::uint64_t first_slot, std::size_t position) const noexcept {
        switch (kind_) {
        case Kind::all_valid:
            return true;
        case Kind::all_invalid:
            return false;
        case Kind::index:
            return positions_->contains(static_cast<row_id_t>(first_slot + position));
        case Kind::bitmap: {
            const auto bit = bit_offset_ + position;
            return (bits_[bit / 8] & (std::uint8_t{1} << (bit % 8))) != 0;
        }
        case Kind::callback:
            return contains_(context_, first_slot, position);
        }
        std::terminate();
    }

    std::size_t first_invalid(std::uint64_t first_slot, std::size_t count) const noexcept {
        switch (kind_) {
        case Kind::all_valid:
            return count;
        case Kind::all_invalid:
            return 0;
        case Kind::bitmap:
        case Kind::callback:
            for (auto position = std::size_t{}; position != count; ++position)
                if (!contains(first_slot, position))
                    return position;
            return count;
        case Kind::index: {
            const auto first = static_cast<util::index<>::size_type>(first_slot);
            const auto end = static_cast<util::index<>::size_type>(first_slot + count);
            return positions_->first_unset(first, end) - first;
        }
        }
        std::terminate();
    }

    ColumnBatchValidity subspan(std::size_t offset) const noexcept {
        auto result = *this;
        if (result.kind_ == Kind::bitmap)
            result.bit_offset_ += offset;
        return result;
    }

    friend bool operator==(const ColumnBatchValidity &, const ColumnBatchValidity &) = default;

  private:
    explicit ColumnBatchValidity(Kind kind) noexcept : kind_{kind} {}
    Kind kind_{Kind::all_invalid};
    const util::index<> *positions_{};
    const std::uint8_t *bits_{};
    const void *context_{};
    bool (*contains_)(const void *, std::uint64_t, std::size_t){};
    std::uint64_t bit_offset_{};
};

template <typename T = void> struct ColumnBatch {
    std::uint64_t first_slot{};
    std::size_t size{};
    typename ColumnValueTraits<T>::pointer values{};
    std::uint64_t value_bit_offset{};
    using Validity = ColumnBatchValidity;
    Validity valid;
    [[no_unique_address]] column_batch_source_t<T> source{};

    ColumnBatch() = default;
    ColumnBatch(std::uint64_t first_slot, std::size_t size, typename ColumnValueTraits<T>::pointer values,
                Validity valid) noexcept
        : first_slot{first_slot}, size{size}, values{values}, valid{valid} {}
    ColumnBatch(std::uint64_t first_slot, std::size_t size, typename ColumnValueTraits<T>::pointer values,
                std::uint64_t value_bit_offset, Validity valid) noexcept
        : first_slot{first_slot}, size{size}, values{values}, value_bit_offset{value_bit_offset}, valid{valid} {}
    ColumnBatch(std::uint64_t first_slot, std::size_t size, typename ColumnValueTraits<T>::pointer values,
                Validity valid, column_batch_source_t<T> source) noexcept
        : first_slot{first_slot}, size{size}, values{values}, valid{valid}, source{source} {}

    bool is_valid(std::size_t position) const noexcept { return valid.contains(first_slot, position); }
    bool has_values() const noexcept {
        if constexpr (source_backed_column_type<T>)
            if (source.read_value != nullptr)
                return true;
        return values != nullptr;
    }
    cell_value_t<T> value_at(std::size_t position) const noexcept {
        if constexpr (source_backed_column_type<T>)
            if (source.read_value != nullptr)
                return source.read_value(source.context, source.first_position + position);
        return ColumnValueTraits<T>::read(values, position, value_bit_offset);
    }
    ColumnBatch subspan(std::size_t offset, std::size_t count) const noexcept {
        auto bit_offset = value_bit_offset;
        const auto subspan_values = ColumnValueTraits<T>::subspan(values, offset, bit_offset);
        auto subspan_source = source;
        if constexpr (source_backed_column_type<T>)
            if (subspan_source.read_value != nullptr)
                subspan_source.first_position += offset;
        auto result = ColumnBatch{first_slot + offset, count, subspan_values, bit_offset, valid.subspan(offset)};
        result.source = subspan_source;
        return result;
    }
};

template <> struct ColumnBatch<void> {
    std::uint64_t first_slot{};
    std::size_t size{};
    const void *values{};
    std::uint64_t value_bit_offset{};
    std::size_t value_bits_per_cell{};
    ColumnBatchValidity valid;

    ColumnBatch() = default;
    ColumnBatch(std::uint64_t first_slot, std::size_t size, const void *values, std::uint64_t value_bit_offset,
                std::size_t value_bits_per_cell, ColumnBatchValidity valid) noexcept
        : first_slot{first_slot}, size{size}, values{values}, value_bit_offset{value_bit_offset},
          value_bits_per_cell{value_bits_per_cell}, valid{valid} {}

    bool is_valid(std::size_t position) const noexcept { return valid.contains(first_slot, position); }
    bool has_values() const noexcept { return values != nullptr; }
    ColumnBatch subspan(std::size_t offset, std::size_t count) const noexcept {
        auto result = *this;
        result.first_slot += offset;
        result.size = count;
        result.valid = valid.subspan(offset);
        const auto bit = value_bit_offset + offset * value_bits_per_cell;
        if (values != nullptr) {
            result.values = static_cast<const std::byte *>(values) + bit / 8;
            result.value_bit_offset = bit % 8;
        }
        return result;
    }
};

// Extension runtime implementers use this borrowed read interface to expose non-native scalar storage through
// ColumnView. Node definitions should use ColumnView directly. The source must outlive every view and copied batch.
template <typename T> class ColumnReadSource {
  public:
    using batch_visitor = void (*)(void *, const ColumnBatch<T> &);
    virtual ~ColumnReadSource() = default;
    virtual std::size_t size() const noexcept = 0;
    virtual std::optional<cell_value_t<T>> cell(std::size_t position) const = 0;
    // Batches must cover size() cells in logical order; each valid cell must have stored values or a source reader.
    virtual void visit_batches(void *target, batch_visitor visit) const = 0;
};

template <typename T> struct NativeColumnBatch {
    std::span<const row_id_t> ids;
    ColumnBatch<T> cells;
};

template <typename Left, typename Right> struct AlignedColumnBatch {
    std::span<const row_id_t> ids;
    ColumnBatch<Left> left;
    ColumnBatch<Right> right;
};

template <typename T = void> class ColumnView {
  public:
    using scalar_type = T;
    using cell_type = std::optional<cell_value_t<T>>;
    using Batch = ColumnBatch<T>;
    ColumnView() = default;
    ColumnView(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    ColumnView(const Column<T> &column) noexcept;
    explicit ColumnView(const ColumnReadSource<T> &source) noexcept : source_{std::addressof(source)} {}
    std::size_t size() const noexcept;
    cell_type cell(std::size_t position) const;
    bool is_native() const noexcept { return native_ != nullptr; }
    template <typename Function> void batches(Function &&function) const {
        if (native_ != nullptr) {
            if (rows_ != nullptr)
                native_->for_each_batch(*rows_, visible_rows_,
                                        [&](const auto &batch) { std::invoke(function, batch.cells); });
            else
                native_->for_each_batch(*physical_rows_, visible_rows_,
                                        [&](const auto &batch) { std::invoke(function, batch.cells); });
            return;
        }
        if (snapshot_ != nullptr && snapshot_->size() != 0)
            std::invoke(std::forward<Function>(function),
                        Batch{0, snapshot_->size(), snapshot_->values(), 0,
                              Batch::Validity::from_index(snapshot_->valid())});
        if (source_ != nullptr) {
            auto invoke = [&](const Batch &batch) { std::invoke(function, batch); };
            source_->visit_batches(std::addressof(invoke), [](void *target, const Batch &batch) {
                std::invoke(*static_cast<decltype(invoke) *>(target), batch);
            });
        }
    }

  private:
    template <typename> friend class NativeColumn;
    template <typename> friend class NativeColumnData;
    template <typename> friend class Column;
    template <typename> friend struct ColumnStorage;
    template <typename> friend struct bg::scalar_storage_traits;
    explicit ColumnView(const ColumnStorage<T> &storage) noexcept;
    explicit ColumnView(const ColumnSnapshot<T> *snapshot) noexcept : snapshot_{snapshot} {}
    const NativeColumnData<T> *native_{};
    const NativeRows *rows_{};
    const NativeRowSlots *physical_rows_{};
    const ColumnSnapshot<T> *snapshot_{};
    const ColumnReadSource<T> *source_{};
    RowSet visible_rows_{};
    RowSet coverage_{};
};

template <> class ColumnView<void> {
  public:
    ColumnView() = default;
    ColumnView(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    ColumnView(const ColumnView<bool> &view) : value_{view} {}
    ColumnView(const ColumnView<i64> &view) : value_{view} {}
    ColumnView(const ColumnView<double> &view) : value_{view} {}
    ColumnView(const ColumnView<string> &view) : value_{view} {}
    ColumnView(const ColumnView<symbol> &view) : value_{view} {}

    const ScalarType &cell_type() const {
        return std::visit(
            [](const auto &view) -> const ScalarType & {
                using View = std::remove_cvref_t<decltype(view)>;
                if constexpr (std::same_as<View, std::monostate>)
                    throw std::logic_error{"generic column view has no cell type"};
                else
                    return cpp_scalar_type<typename View::scalar_type>();
            },
            value_);
    }

    std::size_t size() const {
        return std::visit(
            [](const auto &view) -> std::size_t {
                using View = std::remove_cvref_t<decltype(view)>;
                if constexpr (std::same_as<View, std::monostate>)
                    throw std::logic_error{"generic column view has no cell type"};
                else
                    return view.size();
            },
            value_);
    }

    template <typename T> requires bg::frame::cell_type<T> ColumnView<T> view() const {
        if (const auto *view = std::get_if<ColumnView<T>>(&value_))
            return *view;
        if (std::holds_alternative<std::monostate>(value_))
            throw std::logic_error{"generic column view has no cell type"};
        else
            throw std::invalid_argument{"column view type does not match the requested cell type"};
    }

    template <typename T>
        requires bg::frame::cell_type<T>
    std::optional<cell_value_t<T>> cell(std::size_t position) const {
        return view<T>().cell(position);
    }

    template <typename T, typename Function>
        requires bg::frame::cell_type<T>
    void batches(Function &&function) const {
        view<T>().batches(std::forward<Function>(function));
    }

  private:
    std::variant<std::monostate, ColumnView<bool>, ColumnView<i64>, ColumnView<double>, ColumnView<string>,
                 ColumnView<symbol>> value_;
};

template <typename T> struct ColumnStorage {
    static constexpr bool supports_byte_copy_initialization = true;
    ColumnStorage() = default;
    ColumnStorage(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    explicit ColumnStorage(const NativeColumnData<T> &value, const NativeRows &rows) noexcept;
    explicit ColumnStorage(ColumnSnapshot<T> *value) noexcept : snapshot{value} {}
    ColumnStorage(const NativeColumnData<T> &value, const NativeRows &physical_rows, RowSet visible,
                  const util::index<> *added = nullptr, const util::index<> *modified = nullptr,
                  const util::index<> *removed = nullptr, EvaluationTime time = MIN_DT) noexcept;
    ColumnStorage(const NativeColumnData<T> &value, const NativeRowSlots &physical_rows, RowSet visible,
                  const util::index<> &added, const util::index<> &modified, const util::index<> &removed,
                  EvaluationTime time) noexcept
        : native{&value}, physical_rows{&physical_rows}, visible_rows{visible}, coverage{visible},
          added{std::addressof(added)}, modified{std::addressof(modified)}, removed{std::addressof(removed)},
          change_time{time} {}
    const NativeColumnData<T> *native{};
    const NativeRows *rows{};
    const NativeRowSlots *physical_rows{};
    ColumnSnapshot<T> *snapshot{};
    RowSet visible_rows{};
    RowSet coverage{};
    const util::index<> *added{};
    const util::index<> *modified{};
    const util::index<> *removed{};
    EvaluationTime change_time{MIN_DT};
};

class RowDomain {
  public:
    row_id_t acquire(allocator memory) { return allocated_.acquire(memory.memory_resource()); }
    void retire(row_id_t id, allocator memory) { retired_.set(id, memory.memory_resource()); }
    void cancel_retire(row_id_t id) noexcept { retired_.reset(id); }
    void release(row_id_t id) noexcept { allocated_.reset(id); }
    void recycle() {
        for (auto id : retired_)
            allocated_.reset(id);
        retired_.clear();
    }
    void release(allocator memory) noexcept {
        allocated_.release(memory.memory_resource());
        retired_.release(memory.memory_resource());
    }

  private:
    util::occupancy_index allocated_;
    util::index<> retired_;
};

template <typename T> class Column {
  public:
    using cell_type = std::optional<cell_value_t<T>>;
    using value_type = ColumnView<T>;
    Column() = default;
    Column(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    explicit Column(std::vector<cell_type> cells) : snapshot_{ColumnSnapshot<T>::create(allocator{}, cells.size())} {
        try {
            for (auto position = std::size_t{}; position != cells.size(); ++position) {
                if (!cells[position])
                    continue;
                snapshot_->set_valid(static_cast<row_id_t>(position));
                snapshot_->set_value(position, *cells[position]);
            }
        } catch (...) {
            snapshot_->release();
            snapshot_ = nullptr;
            throw;
        }
    }
    Column(std::initializer_list<cell_type> cells) : Column{std::vector<cell_type>{cells}} {}
    Column(const Column &other) noexcept : snapshot_{other.snapshot_} { retain(); }
    Column(Column &&other) noexcept : snapshot_{std::exchange(other.snapshot_, nullptr)} {}
    Column &operator=(const Column &other) noexcept {
        if (this != &other) {
            other.retain();
            release();
            snapshot_ = other.snapshot_;
        }
        return *this;
    }
    Column &operator=(Column &&other) noexcept {
        if (this != &other) {
            release();
            snapshot_ = std::exchange(other.snapshot_, nullptr);
        }
        return *this;
    }
    ~Column() { release(); }
    std::size_t size() const noexcept { return snapshot_ == nullptr ? 0 : snapshot_->size(); }
    cell_type cell(std::size_t position) const { return view().cell(position); }
    ColumnView<T> view() const noexcept { return ColumnView<T>{snapshot_}; }
    ColumnStorage<T> storage() const noexcept { return ColumnStorage<T>{snapshot_}; }

  private:
    void retain() const noexcept {
        if (snapshot_ != nullptr)
            snapshot_->retain();
    }
    void release() noexcept {
        if (snapshot_ != nullptr)
            snapshot_->release();
    }
    template <typename> friend class ColumnView;
    ColumnSnapshot<T> *snapshot_{};
};

class NativeRowSlots {
  public:
    static constexpr bool supports_byte_copy_initialization = true;
    NativeRowSlots() noexcept = default;
    NativeRowSlots(const NativeRowSlots &) = delete;

    std::size_t size() const noexcept { return live_slots_.size(); }
    std::uint64_t layout_revision() const noexcept { return layout_revision_; }
    bool contains(row_id_t id) const noexcept { return id < id_extent_ && ids_to_slots_[id] != no_row; }
    row_id_t id_at(std::size_t position) const {
        if (position >= size())
            throw std::out_of_range{"frame row position"};
        auto remaining = position;
        for (auto slot : live_slots_)
            if (remaining-- == 0)
                return slots_to_ids_[slot];
        std::terminate();
    }
    row_id_t slot_of(row_id_t id) const {
        if (!contains(id))
            throw std::out_of_range{"frame row ID"};
        return ids_to_slots_[id];
    }
    row_id_t id_at_slot(row_id_t slot) const noexcept { return slots_to_ids_[slot]; }
    const util::occupancy_index &live_slots() const noexcept { return live_slots_; }
    row_id_t insert(row_id_t id, allocator memory) {
        if (id == no_row)
            throw std::out_of_range{"frame row ID"};
        if (contains(id))
            throw std::invalid_argument{"frame row ID is already mapped"};
        const auto slot = live_slots_.acquire(memory.memory_resource());
        auto release_slot = util::scope_exit{[&]() noexcept { live_slots_.reset(slot); }};
        ensure_mapping(id, slot, memory);
        ids_to_slots_[id] = slot;
        slots_to_ids_[slot] = id;
        release_slot.release();
        return slot;
    }

    void erase(row_id_t id) noexcept {
        if (!contains(id))
            return;
        const auto slot = ids_to_slots_[id];
        live_slots_.reset(slot);
        ids_to_slots_[id] = no_row;
        slots_to_ids_[slot] = no_row;
    }

    void release(allocator memory) noexcept {
        live_slots_.release(memory.memory_resource());
        slots_to_ids_.release_prefix(slot_extent_, memory.memory_resource());
        ids_to_slots_.release_prefix(id_extent_, memory.memory_resource());
        slot_extent_ = 0;
        id_extent_ = 0;
    }

    util::storage_block ids_block_containing(row_id_t slot) const noexcept {
        return slots_to_ids_.block_containing(slot);
    }

    template <typename CompatibleBlockEnd, typename Relocate>
    void compact(allocator memory, CompatibleBlockEnd &&compatible_block_end, Relocate &&relocate) {
        struct SlotRun {
            row_id_t first{};
            row_id_t end{};
        };

        auto runs = std::pmr::vector<SlotRun>{std::addressof(memory.memory_resource())};
        auto live_slot = live_slots_.begin();
        const auto live_end = live_slots_.end();
        auto revision_advanced = false;
        auto block_first = std::uint64_t{};
        while (block_first < slot_extent_) {
            const auto mapping_block = ids_block_containing(static_cast<row_id_t>(block_first));
            const auto block_end = std::min({std::uint64_t{slot_extent_}, mapping_block.end,
                                             std::invoke(compatible_block_end,
                                                         static_cast<row_id_t>(block_first))});
            assert(block_end > block_first);

            runs.clear();
            while (live_slot != live_end && *live_slot < block_end) {
                const auto slot = *live_slot;
                ++live_slot;
                if (!runs.empty() && runs.back().end == slot) {
                    ++runs.back().end;
                } else {
                    runs.push_back({slot, static_cast<row_id_t>(slot + 1)});
                }
            }

            if (runs.size() > 1) {
                auto anchor = std::size_t{};
                for (auto index = std::size_t{1}; index != runs.size(); ++index)
                    if (runs[index].end - runs[index].first > runs[anchor].end - runs[anchor].first)
                        anchor = index;

                auto destination = std::uint64_t{runs[anchor].first};
                for (auto index = anchor; index != 0;) {
                    const auto run = runs[--index];
                    const auto count = static_cast<std::uint32_t>(run.end - run.first);
                    destination -= count;
                    relocate_run(run.first, static_cast<row_id_t>(destination), count, revision_advanced, relocate);
                }

                destination = runs[anchor].end;
                for (auto index = anchor + 1; index != runs.size(); ++index) {
                    const auto run = runs[index];
                    const auto count = static_cast<std::uint32_t>(run.end - run.first);
                    relocate_run(run.first, static_cast<row_id_t>(destination), count, revision_advanced, relocate);
                    destination += count;
                }
            }
            block_first = block_end;
        }
    }

  private:
    template <typename Relocate>
    void relocate_run(row_id_t source, row_id_t destination, std::uint32_t count, bool &revision_advanced,
                      Relocate &relocate) {
        if (source == destination)
            return;
        std::invoke(relocate, source, destination, count,
                    [&]() noexcept { move_range(source, destination, count, revision_advanced); });
    }

    void move_range(row_id_t source, row_id_t destination, std::uint32_t count,
                    bool &revision_advanced) noexcept {
        assert(count != 0 && source != destination);
        const auto source_end = static_cast<std::uint64_t>(source) + count;
        const auto destination_end = static_cast<std::uint64_t>(destination) + count;
        assert(source_end <= slot_extent_ && destination_end <= slot_extent_);
        for (auto slot = source; slot != source_end; ++slot)
            assert(live_slots_.contains(slot));
        for (auto slot = std::uint64_t{destination}; slot != destination_end; ++slot) {
            assert(live_slots_.contains(static_cast<row_id_t>(slot)) == (slot >= source && slot < source_end));
        }

        if (!revision_advanced) {
            ++layout_revision_;
            revision_advanced = true;
        }

        const auto move_id = [&](row_id_t source_slot, row_id_t destination_slot) noexcept {
            const auto id = slots_to_ids_[source_slot];
            assert(id != no_row && ids_to_slots_[id] == source_slot);
            slots_to_ids_[destination_slot] = id;
            ids_to_slots_[id] = destination_slot;
        };
        if (destination < source) {
            for (auto offset = std::uint32_t{}; offset != count; ++offset)
                move_id(static_cast<row_id_t>(source + offset), static_cast<row_id_t>(destination + offset));
        } else {
            for (auto offset = count; offset != 0; --offset)
                move_id(static_cast<row_id_t>(source + offset - 1), static_cast<row_id_t>(destination + offset - 1));
        }

        if (destination < source) {
            for (auto slot = std::max<std::uint64_t>(source, destination_end); slot != source_end; ++slot)
                slots_to_ids_[static_cast<row_id_t>(slot)] = no_row;
        } else {
            for (auto slot = source; slot != std::min(source_end, std::uint64_t{destination}); ++slot)
                slots_to_ids_[static_cast<row_id_t>(slot)] = no_row;
        }

        live_slots_.relocate(source, destination, count);
    }

    void ensure_mapping(row_id_t id, row_id_t slot, allocator memory) {
        while (id_extent_ <= id) {
            ids_to_slots_.emplace_at(id_extent_, memory.memory_resource(), no_row);
            ++id_extent_;
        }
        while (slot_extent_ <= slot) {
            slots_to_ids_.emplace_at(slot_extent_, memory.memory_resource(), no_row);
            ++slot_extent_;
        }
    }

    util::occupancy_index live_slots_;
    util::sparse_bunch<row_id_t> slots_to_ids_, ids_to_slots_;
    row_id_t slot_extent_{}, id_extent_{};
    std::uint64_t layout_revision_{};
};

class NativeRows {
  public:
    static constexpr bool supports_byte_copy_initialization = true;
    NativeRows() noexcept = default;
    NativeRows(const NativeRows &) = delete;

    const void *identity() const noexcept { return std::addressof(domain_); }
    std::size_t size() const noexcept { return slots_.size(); }
    bool contains(row_id_t id) const noexcept { return slots_.contains(id); }
    row_id_t id_at(std::size_t position) const { return slots_.id_at(position); }
    row_id_t slot_of(row_id_t id) const { return slots_.slot_of(id); }
    row_id_t id_at_slot(row_id_t slot) const noexcept { return slots_.id_at_slot(slot); }
    const util::occupancy_index &live_slots() const noexcept { return slots_.live_slots(); }
    const util::index<> &added() const noexcept { return added_; }
    const util::index<> &removed() const noexcept { return removed_; }
    EvaluationTime change_time() const noexcept { return change_time_; }
    NativeRowSlots &slots() noexcept { return slots_; }
    const NativeRowSlots &slots() const noexcept { return slots_; }

    bool reset_changes(EvaluationTime time) {
        if (time == change_time_)
            return false;
        domain_.recycle();
        added_.clear();
        removed_.clear();
        change_time_ = time;
        return true;
    }

    row_id_t add(EvaluationTime time, allocator memory) {
        reset_changes(time);
        const auto id = domain_.acquire(memory);
        auto release_id = util::scope_exit{[&]() noexcept { domain_.release(id); }};
        slots_.insert(id, memory);
        auto erase_slot = util::scope_exit{[&]() noexcept { slots_.erase(id); }};
        added_.set(id, memory.memory_resource());
        erase_slot.release();
        release_id.release();
        return id;
    }

    void rollback_add(row_id_t id) noexcept {
        slots_.erase(id);
        added_.reset(id);
        domain_.release(id);
    }

    bool erase(row_id_t id, EvaluationTime time, allocator memory) {
        reset_changes(time);
        if (!contains(id))
            return false;
        domain_.retire(id, memory);
        auto cancel_retirement = util::scope_exit{[&]() noexcept { domain_.cancel_retire(id); }};
        const auto add_removal = !added_.contains(id);
        if (add_removal)
            removed_.set(id, memory.memory_resource());
        auto remove_change = util::scope_exit{[&]() noexcept {
            if (add_removal)
                removed_.reset(id);
        }};
        slots_.erase(id);
        added_.reset(id);
        remove_change.release();
        cancel_retirement.release();
        return true;
    }

    void release(allocator memory) noexcept {
        slots_.release(memory);
        domain_.release(memory);
        added_.release(memory.memory_resource());
        removed_.release(memory.memory_resource());
        change_time_ = MIN_DT;
    }

    util::storage_block ids_block_containing(row_id_t slot) const noexcept {
        return slots_.ids_block_containing(slot);
    }

  private:
    RowDomain domain_;
    NativeRowSlots slots_;
    util::index<> added_, removed_;
    EvaluationTime change_time_{MIN_DT};
};

template <typename T>
ColumnStorage<T>::ColumnStorage(const NativeColumnData<T> &value, const NativeRows &rows) noexcept
    : native{&value}, rows{&rows}, physical_rows{&rows.slots()}, visible_rows{RowSet::native(rows)},
      coverage{visible_rows} {}

template <typename T>
ColumnStorage<T>::ColumnStorage(const NativeColumnData<T> &value, const NativeRows &physical_rows, RowSet visible,
                                const util::index<> *added, const util::index<> *modified,
                                const util::index<> *removed, EvaluationTime time) noexcept
    : native{&value}, rows{&physical_rows}, physical_rows{&physical_rows.slots()}, visible_rows{visible},
      coverage{visible}, added{added}, modified{modified}, removed{removed}, change_time{time} {}

inline RowSet RowSet::native(const NativeRows &rows) noexcept {
    return RowSet{
        .context = std::addressof(rows),
        .native_rows = std::addressof(rows),
        .identity = rows.identity(),
        .size = [](const void *context) noexcept { return static_cast<const NativeRows *>(context)->size(); },
        .contains = [](const void *context, row_id_t id) noexcept {
            return static_cast<const NativeRows *>(context)->contains(id);
        },
        .id_at = [](const void *context, std::size_t position) {
            return static_cast<const NativeRows *>(context)->id_at(position);
        },
        .for_each = [](const void *context, void *target, visit_function visit) {
            const auto &source = *static_cast<const NativeRows *>(context);
            for (auto slot : source.live_slots())
                visit(target, source.id_at_slot(slot));
        },
        .for_each_span = [](const void *context, void *target, span_visit_function visit) {
            const auto &source = *static_cast<const NativeRows *>(context);
            const auto &physical_rows = source.slots();
            source.live_slots().for_each_run([&](row_id_t first, row_id_t end) {
                while (first != end) {
                    const auto ids_block = physical_rows.ids_block_containing(first);
                    const auto batch_end = static_cast<row_id_t>(std::min<std::uint64_t>(end, ids_block.end));
                    const auto *ids = static_cast<const row_id_t *>(ids_block.data) + first - ids_block.first;
                    const auto span = RowSetSpan{std::span<const row_id_t>{ids, batch_end - first},
                                                 std::addressof(physical_rows), first};
                    visit(target, span);
                    first = batch_end;
                }
            });
        }};
}

template <cell_type T> class ColumnValueStorage {
  public:
    using stored_type = cell_stored_t<T>;
    using value_type = cell_value_t<T>;
    static_assert(std::is_trivially_destructible_v<stored_type>);

    void set(row_id_t slot, value_type value, allocator memory) {
        values_.emplace_at_with(slot, memory.memory_resource(), [&](stored_type *stored) {
            scalar_storage_traits<T>::construct(stored, value, memory);
        });
    }
    void assign(row_id_t slot, value_type value, allocator memory) {
        scalar_storage_traits<T>::assign(values_[slot], value, memory);
    }
    void clear(row_id_t slot, allocator memory) noexcept {
        values_.erase_at_with(slot, memory.memory_resource(), [&](stored_type *stored) noexcept {
            scalar_storage_traits<T>::destroy(stored, memory);
        });
    }
    value_type value(row_id_t slot) const noexcept { return scalar_storage_traits<T>::read(values_[slot]); }
    const stored_type &stored(row_id_t slot) const noexcept { return values_[slot]; }
    void prepare_relocation(row_id_t, row_id_t, std::uint32_t, const util::index<> &, allocator) {}
    void relocate(row_id_t source, row_id_t destination, std::uint32_t count) noexcept {
        values_.relocate(source, destination, count);
    }
    util::storage_block block_containing(row_id_t slot) const noexcept { return values_.block_containing(slot); }
    ColumnBatch<T> batch(row_id_t first, std::size_t size) const noexcept {
        const auto block = block_containing(first);
        const auto *values = block.data == nullptr
                                 ? nullptr
                                 : static_cast<const stored_type *>(block.data) + first - block.first;
        return {first, size, values, 0, ColumnBatch<T>::Validity::all_invalid()};
    }
    void release(const util::index<> &valid, allocator memory) noexcept {
        values_.release_with(valid, memory.memory_resource(), [&](stored_type *stored) noexcept {
            scalar_storage_traits<T>::destroy(stored, memory);
        });
    }

  private:
    util::sparse_bunch<stored_type> values_;
};

template <> class ColumnValueStorage<bool> {
  public:
    void set(row_id_t slot, bool value, allocator memory) {
        const auto byte = static_cast<row_id_t>(slot / 8);
        ensure_byte(byte, memory);
        assign(slot, value);
    }
    void assign(row_id_t slot, bool value) noexcept {
        auto &byte = bytes_[slot / 8];
        const auto mask = std::uint8_t{1} << (slot % 8);
        if (value)
            byte |= mask;
        else
            byte &= static_cast<std::uint8_t>(~mask);
    }
    void prepare_relocation(row_id_t source, row_id_t destination, std::uint32_t count,
                            const util::index<> &valid_cells, allocator memory) {
        const auto source_end = static_cast<std::uint64_t>(source) + count;
        for (auto slot = valid_cells.first_set(source); slot < source_end;
             slot = valid_cells.first_set(slot + 1))
            ensure_byte(static_cast<row_id_t>((destination + slot - source) / 8), memory);
    }
    void relocate(row_id_t source, row_id_t destination, std::uint32_t count,
                  const util::index<> &valid_cells) noexcept {
        if (destination < source) {
            for (auto offset = std::uint32_t{}; offset != count; ++offset) {
                const auto source_slot = static_cast<row_id_t>(source + offset);
                if (valid_cells.contains(source_slot))
                    assign(static_cast<row_id_t>(destination + offset), value(source_slot));
            }
            return;
        }
        for (auto offset = count; offset != 0; --offset) {
            const auto source_slot = static_cast<row_id_t>(source + offset - 1);
            if (valid_cells.contains(source_slot))
                assign(static_cast<row_id_t>(destination + offset - 1), value(source_slot));
        }
    }
    void clear(row_id_t, allocator) noexcept {}
    bool value(row_id_t slot) const noexcept {
        const auto byte = bytes_[slot / 8];
        return (byte & (std::uint8_t{1} << (slot % 8))) != 0;
    }
    util::storage_block block_containing(row_id_t slot) const noexcept {
        const auto block = bytes_.block_containing(slot / 8);
        return {block.first * 8, block.end * 8, block.data};
    }
    ColumnBatch<bool> batch(row_id_t first, std::size_t size) const noexcept {
        const auto block = block_containing(first);
        const auto bit_offset = static_cast<std::uint64_t>(first) - block.first;
        const auto *values = static_cast<const std::uint8_t *>(block.data);
        return {first, size, values == nullptr ? nullptr : values + bit_offset / 8, bit_offset % 8,
                ColumnBatch<bool>::Validity::all_invalid()};
    }
    void release(const util::index<> &, allocator memory) noexcept {
        bytes_.release(allocated_bytes_, memory.memory_resource());
        allocated_bytes_.release(memory.memory_resource());
    }

  private:
    void ensure_byte(row_id_t byte, allocator memory) {
        if (allocated_bytes_.contains(byte))
            return;
        bytes_.emplace_at(byte, memory.memory_resource(), std::uint8_t{});
        auto rollback = util::scope_exit{[&]() noexcept { bytes_.erase_at(byte, memory.memory_resource()); }};
        allocated_bytes_.set(byte, memory.memory_resource());
        rollback.release();
    }

    util::occupancy_index allocated_bytes_;
    util::sparse_bunch<std::uint8_t> bytes_;
};

template <typename T> class NativeColumnData {
  public:
    using cell_type = std::optional<cell_value_t<T>>;
    using Batch = NativeColumnBatch<T>;
    static constexpr bool supports_byte_copy_initialization = true;
    NativeColumnData() noexcept = default;
    NativeColumnData(const NativeColumnData &) = delete;
    const util::index<> &added(const NativeRows &rows) const noexcept { return rows.added(); }
    const util::index<> &modified() const noexcept { return modified_; }
    const util::index<> &removed(const NativeRows &rows) const noexcept { return rows.removed(); }
    std::size_t size(const NativeRows &rows) const noexcept { return rows.size(); }
    row_id_t id_at(const NativeRows &rows, std::size_t position) const { return rows.id_at(position); }
    bool contains(const NativeRows &rows, row_id_t id) const noexcept { return rows.contains(id); }
    cell_type cell(const NativeRows &rows, row_id_t id) const { return cell(rows.slots(), id); }
    cell_type cell(const NativeRowSlots &rows, row_id_t id) const {
        const auto slot = rows.slot_of(id);
        return valid_cells_.contains(slot) ? cell_type{cells_.value(slot)} : std::nullopt;
    }
    const cell_stored_t<T> &stored(const NativeRowSlots &rows, row_id_t id) const noexcept {
        return cells_.stored(rows.slot_of(id));
    }
    cell_type cell_at(const NativeRows &rows, std::size_t position) const { return cell(rows, id_at(rows, position)); }
    const util::occupancy_index &live_slots(const NativeRows &rows) const noexcept { return rows.live_slots(); }
    const util::index<> &valid_cells() const noexcept { return valid_cells_; }
    EvaluationTime change_time(const NativeRows &rows) const noexcept { return rows.change_time(); }
    util::storage_block block_containing(const NativeRows &rows, row_id_t id) const noexcept {
        return block_containing(rows.slots(), id);
    }
    util::storage_block block_containing(const NativeRowSlots &rows, row_id_t id) const noexcept {
        return rows.contains(id) ? cells_.block_containing(rows.slot_of(id)) : util::storage_block{};
    }
    util::storage_block block_containing_slot(row_id_t slot) const noexcept { return cells_.block_containing(slot); }
    void reset_changes() noexcept { modified_.clear(); }
    void add(NativeRows &rows, row_id_t id, cell_type value, allocator memory) {
        add(rows.slots(), id, value, memory);
    }
    void add(const NativeRowSlots &rows, row_id_t id, cell_type value, allocator memory) {
        const auto slot = rows.slot_of(id);
        if (!value)
            return;
        cells_.set(slot, *value, memory);
        auto erase_cell = util::scope_exit{[&]() noexcept {
            valid_cells_.reset(slot);
            cells_.clear(slot, memory);
        }};
        valid_cells_.set(slot, memory.memory_resource());
        erase_cell.release();
    }
    bool set(const NativeRows &rows, row_id_t id, cell_type value, allocator memory) {
        return set(rows.slots(), rows.added(), id, value, memory);
    }
    bool set(const NativeRowSlots &rows, const util::index<> &added, row_id_t id, cell_type value,
             allocator memory) {
        if (!rows.contains(id))
            throw std::out_of_range{"frame row ID"};
        const auto slot = rows.slot_of(id);
        const auto add_change = !added.contains(id) && !modified_.contains(id);
        if (add_change)
            modified_.set(id, memory.memory_resource());
        auto remove_change = util::scope_exit{[&]() noexcept {
            if (add_change)
                modified_.reset(id);
        }};
        if (valid_cells_.contains(slot) && value) {
            if constexpr (std::same_as<T, bool>)
                cells_.assign(slot, *value);
            else
                cells_.assign(slot, *value, memory);
        } else if (valid_cells_.contains(slot)) {
            cells_.clear(slot, memory);
            valid_cells_.reset(slot);
        }
        if (!valid_cells_.contains(slot) && value) {
            cells_.set(slot, *value, memory);
            auto erase_cell = util::scope_exit{[&]() noexcept {
                valid_cells_.reset(slot);
                cells_.clear(slot, memory);
            }};
            valid_cells_.set(slot, memory.memory_resource());
            erase_cell.release();
        }
        remove_change.release();
        return true;
    }
    void clear_physical_slot(row_id_t slot, allocator memory) noexcept {
        if (valid_cells_.contains(slot)) {
            cells_.clear(slot, memory);
            valid_cells_.reset(slot);
        }
    }
    void erase_slot(row_id_t slot, row_id_t id, allocator memory) {
        clear_physical_slot(slot, memory);
        modified_.reset(id);
    }
    void prepare_relocation(row_id_t source, row_id_t destination, std::uint32_t count, allocator memory) {
        if (source == destination || count == 0)
            return;

        const auto source_end = static_cast<std::uint64_t>(source) + count;
        if (valid_cells_.first_set(source) >= source_end)
            return;

        const auto destination_end = static_cast<std::uint64_t>(destination) + count;
        valid_cells_.grow(static_cast<util::index<>::size_type>(std::max(source_end, destination_end)),
                          memory.memory_resource());
        cells_.prepare_relocation(source, destination, count, valid_cells_, memory);
    }
    void relocate(row_id_t source, row_id_t destination, std::uint32_t count) noexcept {
        if (source == destination || count == 0)
            return;

        const auto source_end = static_cast<row_id_t>(source + count);
        if (valid_cells_.first_set(source) >= source_end)
            return;

        if constexpr (std::same_as<T, bool>) {
            cells_.relocate(source, destination, count, valid_cells_);
        } else if (valid_cells_.first_unset(source, source_end) == source_end) {
            cells_.relocate(source, destination, count);
        } else if (destination < source) {
            auto first = valid_cells_.first_set(source);
            while (first < source_end) {
                const auto end = valid_cells_.first_unset(first, source_end);
                cells_.relocate(first, static_cast<row_id_t>(destination + first - source), end - first);
                first = valid_cells_.first_set(end);
            }
        } else {
            auto end = source_end;
            while (end != source) {
                while (end != source && !valid_cells_.contains(end - 1))
                    --end;
                if (end == source)
                    break;
                const auto run_end = end;
                while (end != source && valid_cells_.contains(end - 1))
                    --end;
                cells_.relocate(end, static_cast<row_id_t>(destination + end - source), run_end - end);
            }
        }
        valid_cells_.relocate(source, destination, count);
    }
    void clear(const NativeRows &rows, allocator memory) {
        clear(rows.slots(), memory);
    }
    void clear(const NativeRowSlots &rows, allocator memory) {
        for (const auto slot : rows.live_slots()) {
            if (valid_cells_.contains(slot)) {
                erase_slot(slot, rows.id_at_slot(slot), memory);
            }
        }
    }
    void release(allocator memory) noexcept {
        cells_.release(valid_cells_, memory);
        valid_cells_.release(memory.memory_resource());
        modified_.release(memory.memory_resource());
    }
    template <typename Function> void for_each_batch(const NativeRows &rows, Function &&function) const {
        rows.live_slots().for_each_run([&](row_id_t first, row_id_t end) {
            while (first != end) {
                const auto cell_block = cells_.block_containing(first);
                const auto id_block = rows.ids_block_containing(first);
                const auto batch_end =
                    static_cast<row_id_t>(std::min({std::uint64_t{end}, cell_block.end, id_block.end}));
                auto cells = this->cells_.batch(first, batch_end - first);
                cells.valid = ColumnBatch<T>::Validity::from_index(valid_cells_);
                const auto ids = std::span<const row_id_t>{
                    static_cast<const row_id_t *>(id_block.data) + first - id_block.first, batch_end - first};
                std::invoke(function, Batch{ids, cells});
                first = batch_end;
            }
        });
    }

    template <typename Function> void for_each_batch(const NativeRowSlots &rows, const RowSet &visible_rows,
                                                     Function &&function) const {
        auto has_batch = false;
        auto first_slot = row_id_t{};
        auto end_slot = std::uint64_t{};
        auto cells_block = util::storage_block{};
        auto ids_block = util::storage_block{};
        const auto flush = [&]() {
            if (!has_batch)
                return;

            const auto size = static_cast<std::size_t>(end_slot - first_slot);
            auto cells = this->cells_.batch(first_slot, size);
            cells.valid = ColumnBatch<T>::Validity::from_index(valid_cells_);
            const auto *ids = static_cast<const row_id_t *>(ids_block.data) + first_slot - ids_block.first;
            std::invoke(function,
                        Batch{std::span<const row_id_t>{ids, size},
                              cells});
            has_batch = false;
        };

        const auto append_id = [&](row_id_t id) {
            const auto slot = rows.slot_of(id);
            if (has_batch && static_cast<std::uint64_t>(slot) == end_slot && end_slot < cells_block.end &&
                end_slot < ids_block.end) {
                ++end_slot;
                return;
            }

            flush();
            cells_block = cells_.block_containing(slot);
            ids_block = rows.ids_block_containing(slot);
            has_batch = true;
            first_slot = slot;
            end_slot = static_cast<std::uint64_t>(slot) + 1;
        };
        visible_rows.each_span([&](const RowSetSpan &span) {
            if (span.physical_rows != std::addressof(rows)) {
                for (auto id : span.ids)
                    append_id(id);
                return;
            }

            flush();
            auto offset = std::size_t{};
            while (offset != span.ids.size()) {
                const auto first = static_cast<row_id_t>(static_cast<std::uint64_t>(span.first_slot) + offset);
                const auto block = cells_.block_containing(first);
                const auto count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(span.ids.size() - offset, block.end - first));
                auto cells = this->cells_.batch(first, count);
                cells.valid = ColumnBatch<T>::Validity::from_index(valid_cells_);
                std::invoke(function, Batch{span.ids.subspan(offset, count), cells});
                offset += count;
            }
        });
        flush();
    }

    template <typename Function> void for_each_batch(const NativeRows &rows, const RowSet &visible_rows,
                                                     Function &&function) const {
        if (visible_rows.native_rows == std::addressof(rows)) {
            for_each_batch(rows, std::forward<Function>(function));
            return;
        }
        for_each_batch(rows.slots(), visible_rows, std::forward<Function>(function));
    }

  private:
    ColumnValueStorage<T> cells_;
    util::index<> valid_cells_;
    util::index<> modified_;
};

template <typename T> class ColumnSeriesView {
  public:
    using cell_type = std::optional<cell_value_t<T>>;
    using Batch = NativeColumnBatch<T>;

    ColumnSeriesView(const NativeColumnData<T> &cells, const NativeRows &rows,
                     const ColumnStorage<T> &storage) noexcept
        : cells_{&cells}, rows_{&rows}, physical_rows_{std::addressof(rows.slots())}, storage_{storage},
          scalar_storage_{std::addressof(storage)} {}
    explicit ColumnSeriesView(const ColumnStorage<T> &storage) noexcept
        : cells_{storage.native}, rows_{storage.rows}, physical_rows_{storage.physical_rows},
          storage_{storage}, scalar_storage_{std::addressof(storage)} {}

    std::size_t size() const noexcept { return storage_.visible_rows.count(); }
    bool contains(row_id_t id) const noexcept {
        return storage_.visible_rows.has(id) && storage_.coverage.has(id) && physical_rows_ != nullptr &&
               physical_rows_->contains(id);
    }
    row_id_t id_at(std::size_t position) const { return storage_.visible_rows.at(position); }
    cell_type cell(row_id_t id) const {
        if (!contains(id))
            throw std::out_of_range{"column row ID"};
        return cells_->cell(*physical_rows_, id);
    }
    cell_type cell_at(std::size_t position) const { return cell(id_at(position)); }
    const void *row_domain() const noexcept { return storage_.visible_rows.identity; }
    const util::index<> &added() const noexcept {
        if (storage_.added != nullptr)
            return *storage_.added;
        return rows_->added();
    }
    const util::index<> &modified() const noexcept {
        if (storage_.modified != nullptr)
            return *storage_.modified;
        return cells_->modified();
    }
    const util::index<> &removed() const noexcept {
        if (storage_.removed != nullptr)
            return *storage_.removed;
        return rows_->removed();
    }
    EvaluationTime change_time() const noexcept {
        return rows_ != nullptr && storage_.change_time == MIN_DT ? rows_->change_time() : storage_.change_time;
    }
    const ColumnStorage<T> &scalar_storage() const noexcept { return *scalar_storage_; }

    template <typename Function> void for_each_batch(Function &&function) const {
        if (rows_ != nullptr)
            cells_->for_each_batch(*rows_, storage_.visible_rows, std::forward<Function>(function));
        else
            cells_->for_each_batch(*physical_rows_, storage_.visible_rows, std::forward<Function>(function));
    }

    // Aligns requested rows by ID and verifies that both views contain them before invoking the callback. It does not
    // prove that the views have equal complete row sets. Batch buffers are borrowed for the callback; invalid cells
    // must not be read.
    template <typename U, typename Function>
    void for_each_aligned_batch(const ColumnSeriesView<U> &other, const RowSet &requested_rows,
                                Function &&function) const;

    template <typename U, typename Function>
    void for_each_aligned_batch(const ColumnSeriesView<U> &other, Function &&function) const {
        for_each_aligned_batch(other, storage_.visible_rows, std::forward<Function>(function));
    }

    // Aligns homogeneous columns over requested rows and passes borrowed value batches in column order.
    template <typename Function>
    static void for_each_aligned_batch(std::span<const ColumnSeriesView<T>> columns, const RowSet &requested_rows,
                                       std::pmr::memory_resource &memory, Function &&function);

  private:
    const NativeColumnData<T> *cells_;
    const NativeRows *rows_;
    const NativeRowSlots *physical_rows_;
    ColumnStorage<T> storage_;
    const ColumnStorage<T> *scalar_storage_;
};

class ErasedColumnSeriesView {
  public:
    template <cell_type T> explicit ErasedColumnSeriesView(const ColumnStorage<T> &storage)
        : native_data_{storage.native}, rows_{storage.rows}, physical_rows_{storage.physical_rows},
          snapshot_{storage.snapshot}, visible_rows_{storage.visible_rows}, coverage_{storage.coverage},
          value_bits_per_cell_{packed_column_value<T> ? 1 : sizeof(cell_stored_t<T>) * 8},
          block_{[](const void *context, row_id_t id) {
              const auto &self = *static_cast<const ErasedColumnSeriesView *>(context);
              return static_cast<const NativeColumnData<T> *>(self.native_data_)->block_containing(
                  *self.physical_rows_, id);
          }},
          block_at_slot_{[](const void *context, row_id_t slot) {
              const auto &self = *static_cast<const ErasedColumnSeriesView *>(context);
              return static_cast<const NativeColumnData<T> *>(self.native_data_)->block_containing_slot(slot);
          }},
          gather_{[](const void *context, std::span<const row_id_t> ids, void *destination,
                     std::span<std::uint8_t> validity) {
              const auto &self = *static_cast<const ErasedColumnSeriesView *>(context);
              const auto &cells = *static_cast<const NativeColumnData<T> *>(self.native_data_);
              for (auto index = std::size_t{}; index != ids.size(); ++index) {
                  if (const auto value = cells.cell(*self.physical_rows_, ids[index])) {
                      if constexpr (std::same_as<T, bool>) {
                          if (*value)
                              static_cast<std::uint8_t *>(destination)[index / 8] |=
                                  std::uint8_t{1} << (index % 8);
                      } else if constexpr (std::same_as<T, string> || std::same_as<T, symbol>) {
                          static_cast<cell_stored_t<T> *>(destination)[index] =
                              cells.stored(*self.physical_rows_, ids[index]);
                      } else {
                          static_cast<T *>(destination)[index] = *value;
                      }
                      validity[index / 8] |= std::uint8_t{1} << (index % 8);
                  }
              }
          }},
          valid_cells_{storage.native == nullptr ? nullptr : std::addressof(storage.native->valid_cells())},
          added_{storage.added != nullptr ? storage.added : (storage.rows == nullptr ? nullptr :
                                                               std::addressof(storage.rows->added()))},
          modified_{storage.modified != nullptr ? storage.modified :
                                                    (storage.native == nullptr ? nullptr :
                                                     std::addressof(storage.native->modified()))},
          removed_{storage.removed != nullptr ? storage.removed : (storage.rows == nullptr ? nullptr :
                                                                     std::addressof(storage.rows->removed()))},
          change_time_{storage.rows != nullptr && storage.change_time == MIN_DT
                           ? storage.rows->change_time()
                           : storage.change_time} {
        static_assert(sizeof(cell_stored_t<T>) <= column_cell_storage_size);
    }

    const void *row_domain() const noexcept { return visible_rows_.identity; }
    const NativeRowSlots *physical_rows() const noexcept { return physical_rows_; }
    const RowSet &visible_rows() const noexcept { return visible_rows_; }
    const RowSet &coverage() const noexcept { return coverage_; }
    const util::index<> &valid_cells() const noexcept { return *valid_cells_; }
    std::size_t value_bits_per_cell() const noexcept { return value_bits_per_cell_; }
    bool has_native() const noexcept { return native_data_ != nullptr; }
    bool same_binding(const ErasedColumnSeriesView &other) const noexcept {
        return native_data_ == other.native_data_ && rows_ == other.rows_ &&
               physical_rows_ == other.physical_rows_ && snapshot_ == other.snapshot_ &&
               visible_rows_ == other.visible_rows_ && coverage_ == other.coverage_;
    }
    const util::index<> &added() const noexcept { return *added_; }
    const util::index<> &modified() const noexcept { return *modified_; }
    const util::index<> &removed() const noexcept { return *removed_; }
    EvaluationTime change_time() const noexcept { return change_time_; }
    util::storage_block block(row_id_t id) const { return block_(this, id); }
    util::storage_block block_at_slot(row_id_t slot) const { return block_at_slot_(this, slot); }
    void gather(std::span<const row_id_t> ids, void *values, std::span<std::uint8_t> validity) const {
        gather_(this, ids, values, validity);
    }
    bool contains(row_id_t id) const {
        return visible_rows_.has(id) && coverage_.has(id) && physical_rows_ != nullptr && physical_rows_->contains(id);
    }

  private:
    const void *native_data_{};
    const NativeRows *rows_{};
    const NativeRowSlots *physical_rows_{};
    const void *snapshot_{};
    RowSet visible_rows_{};
    RowSet coverage_{};
    std::size_t value_bits_per_cell_{};
    util::storage_block (*block_)(const void *, row_id_t){};
    util::storage_block (*block_at_slot_)(const void *, row_id_t){};
    void (*gather_)(const void *, std::span<const row_id_t>, void *, std::span<std::uint8_t>){};
    const util::index<> *valid_cells_{};
    const util::index<> *added_{};
    const util::index<> *modified_{};
    const util::index<> *removed_{};
    EvaluationTime change_time_{MIN_DT};
};

template <typename Function>
void for_each_aligned_column_batch(std::span<const ErasedColumnSeriesView> columns, const RowSet &requested_rows,
                                  std::pmr::memory_resource &memory, Function &&function);

template <typename Function>
void for_each_aligned_column_batch(std::span<const ErasedColumnSeriesView> columns, const RowSet &requested_rows,
                                  std::pmr::memory_resource &memory, Function &&function) {
    if (columns.empty())
        throw std::invalid_argument{"aligned column batches require at least one column"};

    const auto domain = columns.front().row_domain();
    if (domain == nullptr || requested_rows.identity != domain)
        throw std::invalid_argument{"aligned columns must use the same row domain"};
    for (const auto &column : columns)
        if (column.row_domain() != domain || !column.has_native() || column.physical_rows() == nullptr)
            throw std::invalid_argument{"aligned column batches require native columns in one row domain"};

    const auto *physical_rows = columns.front().physical_rows();
    const auto shared_map = std::ranges::all_of(columns, [physical_rows](const auto &column) {
        return column.physical_rows() == physical_rows;
    });
    const auto full_native_rows = shared_map && requested_rows.native_rows != nullptr &&
                                  physical_rows == std::addressof(requested_rows.native_rows->slots()) &&
                                  std::ranges::all_of(columns, [&](const auto &column) {
                                      return requested_rows == column.visible_rows() &&
                                             requested_rows == column.coverage();
                                  });
    if (!full_native_rows) {
        requested_rows.each([&](row_id_t id) {
            for (const auto &column : columns)
                if (!column.contains(id))
                    throw std::invalid_argument{"aligned columns do not contain every requested row"};
        });
    }

    auto batches = std::pmr::vector<ColumnBatch<>>{columns.size(), &memory};
    auto value_blocks = std::pmr::vector<util::storage_block>{columns.size(), &memory};
    const auto invoke_direct = [&](row_id_t first, std::size_t size, std::span<const row_id_t> ids,
                                   std::span<const util::storage_block> blocks) {
        for (auto index = std::size_t{}; index != columns.size(); ++index) {
            const auto &block = blocks[index];
            const auto bit_offset = (first - block.first) * columns[index].value_bits_per_cell();
            const auto *values = block.data == nullptr
                                     ? nullptr
                                     : static_cast<const std::byte *>(block.data) + bit_offset / 8;
            batches[index] = {first, size, values, bit_offset % 8, columns[index].value_bits_per_cell(),
                              ColumnBatchValidity::from_index(columns[index].valid_cells())};
        }
        std::invoke(function, ids, std::span<const ColumnBatch<>>{batches});
    };

    if (shared_map) {
        if (full_native_rows) {
            physical_rows->live_slots().for_each_run([&](row_id_t first, row_id_t end) {
                while (first != end) {
                    const auto id_block = physical_rows->ids_block_containing(first);
                    const auto id = physical_rows->id_at_slot(first);
                    auto batch_end = id_block.end;
                    for (auto index = std::size_t{}; index != columns.size(); ++index) {
                        value_blocks[index] = columns[index].block(id);
                        batch_end = std::min(batch_end, value_blocks[index].end);
                    }
                    batch_end = static_cast<row_id_t>(std::min<std::uint64_t>(end, batch_end));
                    const auto size = static_cast<std::size_t>(batch_end - first);
                    const auto *ids = static_cast<const row_id_t *>(id_block.data) + first - id_block.first;
                    invoke_direct(first, size, std::span<const row_id_t>{ids, size}, value_blocks);
                    first = batch_end;
                }
            });
            return;
        }

        auto has_batch = false;
        auto first_slot = row_id_t{};
        auto end_slot = std::uint64_t{};
        auto id_block = util::storage_block{};
        auto next_blocks = std::pmr::vector<util::storage_block>{columns.size(), &memory};
        const auto flush = [&] {
            if (!has_batch)
                return;
            const auto size = static_cast<std::size_t>(end_slot - first_slot);
            const auto *ids = static_cast<const row_id_t *>(id_block.data) + first_slot - id_block.first;
            invoke_direct(first_slot, size, std::span<const row_id_t>{ids, size}, value_blocks);
            has_batch = false;
        };
        const auto append_id = [&](row_id_t id) {
            const auto slot = physical_rows->slot_of(id);
            const auto next_id_block = physical_rows->ids_block_containing(slot);
            for (auto index = std::size_t{}; index != columns.size(); ++index)
                next_blocks[index] = columns[index].block(id);

            auto same_blocks = has_batch && static_cast<std::uint64_t>(slot) == end_slot &&
                               next_id_block.first == id_block.first && next_id_block.data == id_block.data &&
                               end_slot < id_block.end;
            for (auto index = std::size_t{}; same_blocks && index != columns.size(); ++index) {
                const auto &current = value_blocks[index];
                const auto &next = next_blocks[index];
                same_blocks = next.first == current.first && next.data == current.data && end_slot < current.end;
            }
            if (same_blocks) {
                ++end_slot;
                return;
            }

            flush();
            id_block = next_id_block;
            std::ranges::copy(next_blocks, value_blocks.begin());
            first_slot = slot;
            end_slot = static_cast<std::uint64_t>(slot) + 1;
            has_batch = true;
        };
        requested_rows.each_span([&](const RowSetSpan &span) {
            if (span.physical_rows != physical_rows) {
                for (auto id : span.ids)
                    append_id(id);
                return;
            }

            flush();
            auto offset = std::size_t{};
            while (offset != span.ids.size()) {
                const auto first = static_cast<row_id_t>(static_cast<std::uint64_t>(span.first_slot) + offset);
                auto batch_end = static_cast<std::uint64_t>(first) + span.ids.size() - offset;
                for (auto index = std::size_t{}; index != columns.size(); ++index) {
                    value_blocks[index] = columns[index].block_at_slot(first);
                    batch_end = std::min(batch_end, value_blocks[index].end);
                }
                const auto count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(span.ids.size() - offset, batch_end - first));
                invoke_direct(first, count, span.ids.subspan(offset, count), value_blocks);
                offset += count;
            }
        });
        flush();
        return;
    }

    static constexpr auto capacity = std::size_t{128};
    static constexpr auto validity_bytes = (capacity + 7) / 8;
    static constexpr auto cell_storage_size = column_cell_storage_size;
    struct alignas(std::max_align_t) GatherBuffer {
        std::array<std::byte, cell_storage_size * capacity> values{};
    };
    auto ids = std::array<row_id_t, capacity>{};
    auto values = std::pmr::vector<GatherBuffer>{columns.size(), &memory};
    auto validity = std::pmr::vector<std::array<std::uint8_t, validity_bytes>>{columns.size(), &memory};
    auto size = std::size_t{};
    const auto flush = [&] {
        if (size == 0)
            return;
        for (auto index = std::size_t{}; index != columns.size(); ++index) {
            if (columns[index].value_bits_per_cell() == 1)
                std::memset(values[index].values.data(), 0, (size + 7) / 8);
            columns[index].gather(std::span<const row_id_t>{ids.data(), size}, values[index].values.data(),
                                  validity[index]);
            batches[index] = {0, size, values[index].values.data(), 0, columns[index].value_bits_per_cell(),
                              ColumnBatchValidity::from_bitmap(validity[index].data(), 0)};
        }
        std::invoke(function, std::span<const row_id_t>{ids.data(), size},
                    std::span<const ColumnBatch<>>{batches});
        for (auto &bits : validity)
            bits.fill(0);
        size = 0;
    };
    requested_rows.each([&](row_id_t id) {
        ids[size] = id;
        if (++size == capacity)
            flush();
    });
    flush();
}

template <typename T>
template <typename U, typename Function>
void ColumnSeriesView<T>::for_each_aligned_batch(const ColumnSeriesView<U> &other, const RowSet &requested_rows,
                                                 Function &&function) const {
    auto columns = std::array{ErasedColumnSeriesView{scalar_storage()},
                              ErasedColumnSeriesView{other.scalar_storage()}};
    constexpr auto pair_columns = std::size_t{2};
    constexpr auto pair_capacity = std::size_t{128};
    constexpr auto pair_validity_bytes = (pair_capacity + 7) / 8;
    constexpr auto pair_scratch_size =
        pair_columns * pair_capacity * column_cell_storage_size + pair_columns * pair_validity_bytes +
        pair_columns * sizeof(ColumnBatch<>) + 2 * pair_columns * sizeof(util::storage_block) +
        5 * alignof(std::max_align_t);
    alignas(std::max_align_t) std::array<std::byte, pair_scratch_size> buffer;
    auto memory = std::pmr::monotonic_buffer_resource{buffer.data(), buffer.size(),
                                                       std::pmr::null_memory_resource()};
    for_each_aligned_column_batch(
        std::span<const ErasedColumnSeriesView>{columns}, requested_rows, memory,
        [&](std::span<const row_id_t> ids, std::span<const ColumnBatch<>> batches) {
            const auto left = ColumnBatch<T>{batches[0].first_slot, batches[0].size,
                                             static_cast<typename ColumnValueTraits<T>::pointer>(batches[0].values),
                                             batches[0].value_bit_offset, batches[0].valid};
            const auto right = ColumnBatch<U>{batches[1].first_slot, batches[1].size,
                                              static_cast<typename ColumnValueTraits<U>::pointer>(batches[1].values),
                                              batches[1].value_bit_offset, batches[1].valid};
            std::invoke(function, AlignedColumnBatch<T, U>{ids, left, right});
        });
}

template <typename T>
template <typename Function>
void ColumnSeriesView<T>::for_each_aligned_batch(std::span<const ColumnSeriesView<T>> columns,
                                                 const RowSet &requested_rows,
                                                 std::pmr::memory_resource &memory, Function &&function) {
    auto erased = std::pmr::vector<ErasedColumnSeriesView>{&memory};
    erased.reserve(columns.size());
    for (const auto &column : columns)
        erased.emplace_back(column.scalar_storage());
    auto typed = std::pmr::vector<ColumnBatch<T>>{columns.size(), &memory};
    for_each_aligned_column_batch(
        std::span<const ErasedColumnSeriesView>{erased}, requested_rows, memory,
        [&](std::span<const row_id_t> ids, std::span<const ColumnBatch<>> batches) {
            for (auto index = std::size_t{}; index != batches.size(); ++index)
                typed[index] = {batches[index].first_slot, batches[index].size,
                                static_cast<typename ColumnValueTraits<T>::pointer>(batches[index].values),
                                batches[index].value_bit_offset, batches[index].valid};
            std::invoke(function, ids, std::span<const ColumnBatch<T>>{typed});
        });
}

struct ColumnRowMembership {
    struct Binding {
        bool row_domain_changed{};
        bool row_set_rebound{};
        bool input_changes_current{};
        bool mapping_changed{};
        bool new_cycle{};
        bool reconciled{};
        bool transitioned{};
    };

    struct Mapping {
        const void *native{};
        const NativeRows *rows{};
        const NativeRowSlots *physical_rows{};
        const void *snapshot{};
        const void *visible_context{};
        const void *visible_identity{};
        const void *coverage_context{};
        const void *coverage_identity{};

        friend bool operator==(const Mapping &, const Mapping &) = default;
    };

    SelectedRows previous_rows;
    util::index<> added_rows;
    util::index<> removed_rows;
    Mapping mapping{};
    EvaluationTime delta_time{MIN_DT};
    bool valid{};

    template <typename T, typename OnAdd, typename OnRemove>
    Binding bind(const ColumnStorage<T> &input, bool source_modified, EvaluationTime source_last_updated,
                 EvaluationTime time, allocator memory, OnAdd &&on_add, OnRemove &&on_remove) {
        if (input.native == nullptr || input.physical_rows == nullptr || input.visible_rows.identity == nullptr ||
            (input.rows != nullptr && input.rows->identity() != input.visible_rows.identity))
            throw std::invalid_argument{"TSC view source has no consistent physical row domain"};
        const auto exact_native_coverage = input.rows != nullptr &&
                                           std::addressof(input.rows->slots()) == input.physical_rows &&
                                           input.coverage.context == input.visible_rows.context &&
                                           input.coverage.identity == input.visible_rows.identity;
        const auto exact_coverage = input.coverage.context == input.visible_rows.context &&
                                    input.coverage.identity == input.visible_rows.identity;
        const auto next_mapping = Mapping{input.native, input.rows, input.physical_rows, input.snapshot,
                                          input.visible_rows.context, input.visible_rows.identity, input.coverage.context,
                                          input.coverage.identity};
        const auto mapping_changed = !valid || mapping != next_mapping;
        const auto previous_row_set = previous_rows.source();
        const auto row_domain_changed = previous_row_set.identity != nullptr &&
                                        previous_row_set.identity != input.visible_rows.identity;
        const auto row_set_rebound = !valid || previous_row_set.context != input.visible_rows.context;
        const auto new_cycle = !valid || time != delta_time;
        auto source_view = ColumnSeriesView<T>{input};
        const auto has_source_deltas = input.rows != nullptr ||
                                       (input.added != nullptr && input.modified != nullptr && input.removed != nullptr);
        const auto input_changes_current = source_modified && has_source_deltas &&
                                           source_view.change_time() == source_last_updated;
        if (!exact_native_coverage) {
            const auto validate_row = [&](row_id_t id) {
                if (!input.coverage.has(id) || !input.physical_rows->contains(id))
                    throw std::invalid_argument{"TSC view source does not cover every visible row"};
            };
            const auto full_validation = !exact_coverage || mapping_changed || row_domain_changed || row_set_rebound ||
                                         (source_modified && !input_changes_current);
            if (full_validation) {
                input.visible_rows.each(validate_row);
            } else if (input_changes_current) {
                for (auto id : source_view.added())
                    if (input.visible_rows.has(id))
                        validate_row(id);
            }
        }

        const auto was_valid = valid;
        if (new_cycle) {
            added_rows.clear();
            removed_rows.clear();
            delta_time = time;
        }

        auto transitioned = false;
        const auto remove_row = [&](row_id_t id) {
            std::invoke(on_remove, id);
            if (!added_rows.reset(id))
                removed_rows.set(id, memory.memory_resource());
            previous_rows.exclude(id);
            transitioned = true;
        };
        const auto add_row = [&](row_id_t id) {
            std::invoke(on_add, id);
            if (!removed_rows.reset(id))
                added_rows.set(id, memory.memory_resource());
            previous_rows.include(id, memory);
            transitioned = true;
        };

        if (row_domain_changed) {
            previous_rows.for_each_selected([&](row_id_t id) {
                std::invoke(on_remove, id);
                removed_rows.set(id, memory.memory_resource());
                transitioned = true;
            });
            previous_rows.clear();
            input.visible_rows.each([&](row_id_t id) {
                std::invoke(on_add, id);
                added_rows.set(id, memory.memory_resource());
                transitioned = true;
            });
        } else if (row_set_rebound) {
            previous_rows.for_each_selected([&](row_id_t id) {
                if (!input.visible_rows.has(id))
                    remove_row(id);
            });
            input.visible_rows.each([&](row_id_t id) {
                if (!previous_rows.contains(id))
                    add_row(id);
            });
        } else if (input_changes_current) {
            for (auto id : source_view.removed())
                if (!input.visible_rows.has(id) && previous_rows.contains(id))
                    remove_row(id);
            for (auto id : source_view.added())
                if (input.visible_rows.has(id) && !previous_rows.contains(id))
                    add_row(id);
        } else if (source_modified) {
            previous_rows.for_each_selected([&](row_id_t id) {
                if (!input.visible_rows.has(id))
                    remove_row(id);
            });
            input.visible_rows.each([&](row_id_t id) {
                if (!previous_rows.contains(id))
                    add_row(id);
            });
        }

        previous_rows.bind(input.visible_rows);
        if (row_domain_changed || row_set_rebound)
            input.visible_rows.each([&](row_id_t id) { previous_rows.include(id, memory); });
        mapping = next_mapping;
        valid = true;
        return {row_domain_changed, row_set_rebound, input_changes_current, mapping_changed, new_cycle,
                source_modified && !row_domain_changed && !row_set_rebound && !input_changes_current,
                transitioned || row_domain_changed || !was_valid};
    }

    void begin_cycle(EvaluationTime time) noexcept {
        if (!valid || time == delta_time)
            return;
        added_rows.clear();
        removed_rows.clear();
        delta_time = time;
    }

    void invalidate() noexcept {
        previous_rows.clear();
        previous_rows.bind({});
        added_rows.clear();
        removed_rows.clear();
        mapping = {};
        valid = false;
    }

    void release(allocator memory) noexcept {
        previous_rows.release(memory);
        added_rows.release(memory.memory_resource());
        removed_rows.release(memory.memory_resource());
        mapping = {};
        valid = false;
    }
};

template <typename T> struct ColumnViewOutputStorage {
    ColumnRowMembership membership;
    ColumnStorage<T> scalar;
    util::index<> modified_rows;

    bool bind(const ColumnStorage<T> &input, bool source_modified, EvaluationTime source_last_updated,
              EvaluationTime time, allocator memory) {
        auto source_view = ColumnSeriesView<T>{input};
        const auto binding = membership.bind(input, source_modified, source_last_updated, time, memory,
                                             [](row_id_t) {}, [&](row_id_t id) { modified_rows.reset(id); });
        if (binding.new_cycle)
            modified_rows.clear();

        if (binding.reconciled) {
            input.visible_rows.each([&](row_id_t id) {
                if (!membership.added_rows.contains(id))
                    modified_rows.set(id, memory.memory_resource());
            });
        }
        if (binding.mapping_changed) {
            input.visible_rows.each([&](row_id_t id) {
                if (!membership.added_rows.contains(id))
                    modified_rows.set(id, memory.memory_resource());
            });
        } else if (binding.input_changes_current) {
            for (auto id : source_view.modified())
                if (input.visible_rows.has(id) && !membership.added_rows.contains(id))
                    modified_rows.set(id, memory.memory_resource());
        }

        scalar = input;
        scalar.added = std::addressof(membership.added_rows);
        scalar.modified = std::addressof(modified_rows);
        scalar.removed = std::addressof(membership.removed_rows);
        scalar.change_time = time;
        return binding.transitioned || !modified_rows.empty();
    }

    void invalidate() noexcept {
        membership.invalidate();
        modified_rows.clear();
        scalar = {};
    }

    void release(allocator memory) noexcept {
        membership.release(memory);
        modified_rows.release(memory.memory_resource());
        scalar = {};
    }
};

template <typename T> struct ColumnComputedOutputStorage {
    ColumnRowMembership membership;
    NativeRowSlots physical_rows;
    NativeColumnData<T> cells;
    ColumnStorage<T> scalar;

    template <typename SourceCell>
    bool bind(const ColumnStorage<SourceCell> &input, bool source_modified, EvaluationTime source_last_updated,
              EvaluationTime time, allocator memory) {
        const auto binding = membership.bind(
            input, source_modified, source_last_updated, time, memory,
            [&](row_id_t id) { physical_rows.insert(id, memory); },
            [&](row_id_t id) {
                if (!physical_rows.contains(id))
                    return;
                const auto slot = physical_rows.slot_of(id);
                cells.erase_slot(slot, id, memory);
                physical_rows.erase(id);
            });
        if (binding.new_cycle)
            cells.reset_changes();
        scalar = ColumnStorage<T>{cells, physical_rows, input.visible_rows, membership.added_rows,
                                  cells.modified(), membership.removed_rows, time};
        return binding.transitioned;
    }

    bool set(row_id_t id, std::optional<cell_value_t<T>> value, EvaluationTime time, allocator memory) {
        if (!membership.valid || !scalar.visible_rows.has(id) || !physical_rows.contains(id))
            throw std::out_of_range{"computed TSC row ID"};
        begin_cycle(time);
        const auto changed = cells.set(physical_rows, membership.added_rows, id, value, memory);
        refresh_scalar(time);
        return changed;
    }

    bool set_batch(std::span<const row_id_t> ids, const ColumnBatch<T> &batch,
                   EvaluationTime time, allocator memory) {
        if (batch.size != ids.size())
            throw std::invalid_argument{"computed TSC batch length does not match its row IDs"};
        auto has_valid_cell = false;
        for (std::size_t position = 0; position != ids.size(); ++position) {
            if (!membership.valid || !scalar.visible_rows.has(ids[position]) || !physical_rows.contains(ids[position]))
                throw std::out_of_range{"computed TSC batch row ID"};
            has_valid_cell = has_valid_cell || batch.is_valid(position);
        }
        if (has_valid_cell && !batch.has_values())
            throw std::invalid_argument{"computed TSC batch has valid cells but no values"};
        if (ids.empty())
            return false;

        begin_cycle(time);
        auto changed = false;
        for (std::size_t position = 0; position != ids.size(); ++position) {
            auto value = std::optional<cell_value_t<T>>{};
            if (batch.is_valid(position))
                value = batch.value_at(position);
            changed = cells.set(physical_rows, membership.added_rows, ids[position], value, memory) || changed;
        }
        refresh_scalar(time);
        return changed;
    }

    ColumnSeriesView<T> series_view() const noexcept { return ColumnSeriesView<T>{scalar}; }

    void compact(allocator memory) {
        physical_rows.compact(
            memory, [&](row_id_t slot) { return cells.block_containing_slot(slot).end; },
            [&](row_id_t source, row_id_t destination, std::uint32_t count, auto &&commit_mapping) {
                cells.prepare_relocation(source, destination, count, memory);
                cells.relocate(source, destination, count);
                std::invoke(commit_mapping);
            });
    }

    void invalidate(allocator memory) noexcept {
        cells.clear(physical_rows, memory);
        physical_rows.release(memory);
        membership.invalidate();
        cells.reset_changes();
        scalar = {};
    }

    void release(allocator memory) noexcept {
        cells.release(memory);
        physical_rows.release(memory);
        membership.release(memory);
        scalar = {};
    }

  private:
    void begin_cycle(EvaluationTime time) noexcept {
        const auto previous_time = membership.delta_time;
        membership.begin_cycle(time);
        if (membership.delta_time != previous_time) {
            cells.reset_changes();
            refresh_scalar(time);
        }
    }

    void refresh_scalar(EvaluationTime time) noexcept {
        scalar.added = std::addressof(membership.added_rows);
        scalar.modified = std::addressof(cells.modified());
        scalar.removed = std::addressof(membership.removed_rows);
        scalar.change_time = time;
    }
};

template <typename T> class NativeColumn {
  public:
    using cell_type = std::optional<cell_value_t<T>>;
    using Batch = NativeColumnBatch<T>;
    static constexpr bool supports_byte_copy_initialization = true;
    NativeColumn() noexcept = default;
    NativeColumn(const NativeColumn &) = delete;
    const void *row_domain() const noexcept { return rows_.identity(); }
    const ColumnStorage<T> &scalar_storage() const noexcept { return scalar_; }
    const util::index<> &added() const noexcept { return rows_.added(); }
    const util::index<> &modified() const noexcept { return cells_.modified(); }
    const util::index<> &removed() const noexcept { return rows_.removed(); }
    std::size_t size() const noexcept { return rows_.size(); }
    row_id_t id_at(std::size_t position) const { return rows_.id_at(position); }
    bool contains(row_id_t id) const noexcept { return rows_.contains(id); }
    cell_type cell(row_id_t id) const { return cells_.cell(rows_, id); }
    cell_type cell_at(std::size_t position) const { return cells_.cell_at(rows_, position); }
    const util::occupancy_index &live_slots() const noexcept { return rows_.live_slots(); }
    const util::index<> &valid_cells() const noexcept { return cells_.valid_cells(); }
    EvaluationTime change_time() const noexcept { return rows_.change_time(); }
    ColumnSeriesView<T> series_view() const noexcept { return ColumnSeriesView<T>{cells_, rows_, scalar_}; }
    util::storage_block block_containing(row_id_t id) const noexcept { return cells_.block_containing(rows_, id); }
    row_id_t add(cell_type value, EvaluationTime time, allocator memory) {
        if (rows_.reset_changes(time))
            cells_.reset_changes();
        const auto id = rows_.add(time, memory);
        auto rollback = util::scope_exit{[&]() noexcept { rows_.rollback_add(id); }};
        cells_.add(rows_, id, value, memory);
        scalar_ = ColumnStorage<T>{cells_, rows_};
        rollback.release();
        return id;
    }
    bool set(row_id_t id, cell_type value, EvaluationTime time, allocator memory) {
        if (rows_.reset_changes(time))
            cells_.reset_changes();
        const auto changed = cells_.set(rows_, id, value, memory);
        scalar_ = ColumnStorage<T>{cells_, rows_};
        return changed;
    }
    bool erase(row_id_t id, EvaluationTime time, allocator memory) {
        if (rows_.reset_changes(time))
            cells_.reset_changes();
        if (!rows_.contains(id))
            return false;
        const auto slot = rows_.slot_of(id);
        if (!rows_.erase(id, time, memory))
            return false;
        cells_.erase_slot(slot, id, memory);
        return true;
    }
    void clear(EvaluationTime time, allocator memory) {
        if (rows_.reset_changes(time))
            cells_.reset_changes();
        auto slot_iterator = rows_.live_slots().begin();
        while (slot_iterator != rows_.live_slots().end()) {
            const auto slot = *slot_iterator;
            ++slot_iterator;
            const auto id = rows_.id_at_slot(slot);
            if (!rows_.erase(id, time, memory))
                std::terminate();
            cells_.erase_slot(slot, id, memory);
        }
        scalar_ = ColumnStorage<T>{cells_, rows_};
    }
    void compact(allocator memory) {
        rows_.slots().compact(
            memory, [&](row_id_t slot) { return cells_.block_containing_slot(slot).end; },
            [&](row_id_t source, row_id_t destination, std::uint32_t count, auto &&commit_mapping) {
                cells_.prepare_relocation(source, destination, count, memory);
                cells_.relocate(source, destination, count);
                std::invoke(commit_mapping);
            });
    }
    void release(allocator memory) noexcept {
        cells_.release(memory);
        rows_.release(memory);
        scalar_ = {};
    }
    template <typename Function> void for_each_batch(Function &&function) const {
        cells_.for_each_batch(rows_, std::forward<Function>(function));
    }

  private:
    friend class ColumnView<T>;
    NativeRows rows_;
    NativeColumnData<T> cells_;
    ColumnStorage<T> scalar_;
};

static_assert(util::byte_copy_initializable<NativeColumn<i64>>);

template <typename T> inline std::size_t ColumnView<T>::size() const noexcept {
    if (native_ != nullptr)
        return visible_rows_.count();
    if (snapshot_ != nullptr)
        return snapshot_->size();
    return source_ == nullptr ? 0 : source_->size();
}

template <cell_type T> bool column_values_equal(const ColumnView<T> &left, const ColumnView<T> &right) {
    if (left.size() != right.size())
        return false;

    auto right_batches = std::vector<ColumnBatch<T>>{};
    right.batches([&](const auto &batch) { right_batches.push_back(batch); });

    auto right_index = std::size_t{};
    auto right_offset = std::size_t{};
    auto compared = std::size_t{};
    auto equal = true;
    left.batches([&](const auto &left_batch) {
        for (auto left_offset = std::size_t{}; left_offset != left_batch.size && equal; ++left_offset) {
            while (right_index != right_batches.size() && right_offset == right_batches[right_index].size) {
                ++right_index;
                right_offset = 0;
            }
            if (right_index == right_batches.size()) {
                equal = false;
                return;
            }

            const auto &right_batch = right_batches[right_index];
            const auto left_valid = left_batch.is_valid(left_offset);
            if (left_valid != right_batch.is_valid(right_offset) ||
                (left_valid && left_batch.value_at(left_offset) != right_batch.value_at(right_offset))) {
                equal = false;
                return;
            }
            ++right_offset;
            ++compared;
        }
    });
    while (right_index != right_batches.size() && right_offset == right_batches[right_index].size) {
        ++right_index;
        right_offset = 0;
    }
    return equal && compared == left.size() && right_index == right_batches.size();
}

inline bool column_values_equal(const ColumnView<> &left, const ColumnView<> &right) {
    if (std::addressof(left.cell_type()) != std::addressof(right.cell_type()))
        return false;
    return visit_cell_type(left.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
        return column_values_equal(left.view<Cell>(), right.view<Cell>());
    });
}

template <typename T>
inline ColumnView<T>::ColumnView(const Column<T> &column) noexcept : snapshot_{column.snapshot_} {}
template <typename T>
inline ColumnView<T>::ColumnView(const ColumnStorage<T> &storage) noexcept
    : native_{storage.native}, rows_{storage.rows}, physical_rows_{storage.physical_rows},
      snapshot_{storage.snapshot}, visible_rows_{storage.visible_rows}, coverage_{storage.coverage} {}
template <typename T> inline typename ColumnView<T>::cell_type ColumnView<T>::cell(std::size_t position) const {
    if (native_ != nullptr) {
        const auto id = visible_rows_.at(position);
        if (!coverage_.has(id) || physical_rows_ == nullptr || !physical_rows_->contains(id))
            throw std::out_of_range{"frame column row ID"};
        return native_->cell(*physical_rows_, id);
    }
    if (snapshot_ != nullptr) {
        if (position >= snapshot_->size())
            throw std::out_of_range{"column cell position"};
        return snapshot_->is_valid(position) ? cell_type{snapshot_->value_at(position)} : std::nullopt;
    }
    if (source_ != nullptr)
        return source_->cell(position);
    throw std::out_of_range{"column cell position"};
}
} // namespace bg::frame

namespace bg {
template <typename T> struct scalar_type_traits<frame::Column<T>> {
    static const ScalarType &type() {
        if constexpr (frame::cell_type<T>)
            return frame::column_type(cpp_scalar_type<T>());
        else
            return frame::column_type(scalar_type_traits<T>::type());
    }
};
template <typename T> struct scalar_declaration_traits<frame::Column<T>> {
    static constexpr bool generic = details::scalar_variable<T>;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = frame::cell_type<T> || details::scalar_variable<T>;
};
template <typename T> struct declaration_variable_traits<frame::Column<T>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        declaration_variable_traits<T>::for_each(std::forward<Visitor>(visitor));
    }
};
template <frame::cell_type T> struct scalar_storage_traits<frame::Column<T>> {
    using stored_type = frame::ColumnStorage<T>;
    using value_type = frame::ColumnView<T>;
    static constexpr bool supports_equality = true, supports_hash = false, uses_variable_memory = true;
    static constexpr bool requires_finalization = true, assign_uses_memory_resource = true;
    static value_type read(const stored_type &value) noexcept {
        return value.native != nullptr ? value_type{value} : value_type{value.snapshot};
    }
    static bool equals(value_type left, value_type right) { return frame::column_values_equal(left, right); }
    static void construct(stored_type *destination, value_type source, allocator &memory) {
        std::construct_at(destination, copy(source, memory));
    }
    static void copy_construct(stored_type *destination, const stored_type &source, allocator &memory) {
        construct(destination, read(source), memory);
    }
    static void assign(stored_type &destination, value_type source, allocator &memory) {
        auto replacement = stored_type{copy(source, memory)};
        auto *previous = destination.snapshot;
        destination = replacement;
        if (previous != nullptr)
            previous->release();
    }
    static void destroy(stored_type *value, allocator &) noexcept {
        if (value->snapshot != nullptr)
            value->snapshot->release();
        std::destroy_at(value);
    }

  private:
    static frame::ColumnSnapshot<T> *copy(value_type source, allocator memory) {
        if (source.snapshot_ != nullptr && &source.snapshot_->memory().memory_resource() == &memory.memory_resource()) {
            auto *snapshot = const_cast<frame::ColumnSnapshot<T> *>(source.snapshot_);
            snapshot->retain();
            return snapshot;
        }
        auto *snapshot = frame::ColumnSnapshot<T>::create(memory, source.size());
        try {
            auto position = std::size_t{};
            source.batches([&](const value_type::Batch &batch) {
                for (auto offset = std::size_t{}; offset != batch.size; ++offset, ++position) {
                    if (batch.is_valid(offset)) {
                        snapshot->set_valid(position);
                        if constexpr (std::same_as<T, bool>)
                            snapshot->set_value(position, batch.value_at(offset));
                        else if (batch.values != nullptr)
                            snapshot->copy_stored_value(position, batch.values[offset]);
                        else
                            snapshot->set_value(position, batch.value_at(offset));
                    }
                }
            });
        } catch (...) {
            snapshot->release();
            throw;
        }
        return snapshot;
    }
};

template <details::scalar_variable T> struct scalar_storage_traits<frame::Column<T>> {
    using stored_type = frame::ColumnStorage<T>;
    using value_type = frame::ColumnView<>;
    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = false;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = true;
    static constexpr bool assign_uses_memory_resource = true;

    static value_type read_value(const ScalarData &source) {
        const auto &type = static_cast<const frame::ColumnType &>(source.type());
        return frame::visit_cell_type(type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            return value_type{details::scalar_data_value<frame::Column<Cell>>(source)};
        });
    }
    static bool equals(value_type left, value_type right) { return frame::column_values_equal(left, right); }
};
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::Column<i64>>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::Column<double>>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::Column<bool>>();
} // namespace bg
