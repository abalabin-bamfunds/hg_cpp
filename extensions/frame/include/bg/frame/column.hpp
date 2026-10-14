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

#include <atomic>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
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
concept cell_type = std::same_as<T, i64> || std::same_as<T, double>;

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
struct RowSet {
    using size_function = std::size_t (*)(const void *) noexcept;
    using contains_function = bool (*)(const void *, row_id_t) noexcept;
    using id_at_function = row_id_t (*)(const void *, std::size_t);
    using visit_function = void (*)(void *, row_id_t);
    using for_each_function = void (*)(const void *, void *, visit_function);

    const void *context{};
    const NativeRows *native_rows{};
    const void *identity{};
    size_function size{};
    contains_function contains{};
    id_at_function id_at{};
    for_each_function for_each{};

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
};

class SelectedRows {
  public:
    SelectedRows() noexcept = default;
    SelectedRows(const SelectedRows &) = delete;

    RowSet rows() const noexcept;
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
        valid_.release(memory.memory_resource());
        memory.delete_object(this);
    }
    allocator memory() const noexcept { return memory_; }
    std::size_t size() const noexcept { return values_.size(); }
    const T *values() const noexcept { return values_.data(); }
    T &value_at(std::size_t position) noexcept { return values_[position]; }
    const T &value_at(std::size_t position) const noexcept { return values_[position]; }
    bool is_valid(std::size_t position) const noexcept { return valid_.contains(static_cast<row_id_t>(position)); }
    void set_valid(row_id_t position) { valid_.set(position, memory_.memory_resource()); }
    const util::index<> &valid() const noexcept { return valid_; }

  private:
    std::atomic_size_t references_{1};
    allocator memory_;
    std::pmr::vector<T> values_;
    util::index<> valid_;
};

// Raw values are readable only when is_valid is true. Native null positions need not contain a T object.
template <typename T> struct ColumnBatch {
    std::uint64_t first_slot{};
    std::size_t size{};
    const T *values{};
    class Validity {
      public:
        enum class Kind : std::uint8_t { all_valid, all_invalid, index, bitmap };

        Validity() noexcept = default;
        static Validity all_valid() noexcept { return Validity{Kind::all_valid}; }
        static Validity all_invalid() noexcept { return Validity{Kind::all_invalid}; }
        static Validity from_index(const util::index<> &positions) noexcept {
            auto result = Validity{Kind::index};
            result.positions_ = std::addressof(positions);
            return result;
        }
        static Validity from_bitmap(const std::uint8_t *bits, std::uint64_t offset) noexcept {
            auto result = Validity{Kind::bitmap};
            result.bits_ = bits;
            result.bit_offset_ = offset;
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
            }
            std::terminate();
        }

        friend bool operator==(const Validity &, const Validity &) = default;

      private:
        explicit Validity(Kind kind) noexcept : kind_{kind} {}
        Kind kind_{Kind::all_invalid};
        const util::index<> *positions_{};
        const std::uint8_t *bits_{};
        std::uint64_t bit_offset_{};
    } valid;

    bool is_valid(std::size_t position) const noexcept { return valid.contains(first_slot, position); }
};

// Extension runtime implementers use this borrowed read interface to expose non-native scalar storage through
// ColumnView. Node definitions should use ColumnView directly. The source must outlive every view that refers to it.
template <typename T> class ColumnReadSource {
  public:
    using batch_visitor = void (*)(void *, const ColumnBatch<T> &);
    virtual ~ColumnReadSource() = default;
    virtual std::size_t size() const noexcept = 0;
    virtual std::optional<T> cell(std::size_t position) const = 0;
    virtual void visit_batches(void *target, batch_visitor visit) const = 0;
};

template <typename T> struct NativeColumnBatch {
    std::span<const row_id_t> ids;
    ColumnBatch<T> cells;
};

template <typename T = void> class ColumnView {
  public:
    using cell_type = std::optional<T>;
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
                        Batch{0, snapshot_->size(), snapshot_->values(),
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
    ColumnView(const ColumnView<i64> &view) : value_{view} {}
    ColumnView(const ColumnView<double> &view) : value_{view} {}

    const ScalarType &cell_type() const {
        if (const auto *integer = std::get_if<ColumnView<i64>>(&value_)) {
            static_cast<void>(integer);
            return cpp_scalar_type<i64>();
        }
        if (std::holds_alternative<ColumnView<double>>(value_))
            return cpp_scalar_type<double>();
        throw std::logic_error{"generic column view has no cell type"};
    }

    std::size_t size() const {
        if (const auto *view = std::get_if<ColumnView<i64>>(&value_))
            return view->size();
        if (const auto *view = std::get_if<ColumnView<double>>(&value_))
            return view->size();
        throw std::logic_error{"generic column view has no cell type"};
    }

    template <typename T>
        requires bg::frame::cell_type<T>
    ColumnView<T> view() const {
        if constexpr (std::same_as<T, i64>) {
            if (const auto *view = std::get_if<ColumnView<i64>>(&value_))
                return *view;
        } else if constexpr (std::same_as<T, double>) {
            if (const auto *view = std::get_if<ColumnView<double>>(&value_))
                return *view;
        }
        if (std::holds_alternative<std::monostate>(value_))
            throw std::logic_error{"generic column view has no cell type"};
        else
            throw std::invalid_argument{"column view type does not match the requested cell type"};
    }

    template <typename T>
        requires bg::frame::cell_type<T>
    auto cell(std::size_t position) const {
        return view<T>().cell(position);
    }

    template <typename T, typename Function>
        requires bg::frame::cell_type<T>
    void batches(Function &&function) const {
        view<T>().batches(std::forward<Function>(function));
    }

  private:
    std::variant<std::monostate, ColumnView<i64>, ColumnView<double>> value_;
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
    using cell_type = std::optional<T>;
    using value_type = ColumnView<T>;
    Column() = default;
    Column(bg::details::FieldProbe &probe) noexcept { probe.record(this); }
    explicit Column(std::vector<cell_type> cells) : snapshot_{ColumnSnapshot<T>::create(allocator{}, cells.size())} {
        try {
            for (auto position = std::size_t{}; position != cells.size(); ++position) {
                if (!cells[position])
                    continue;
                snapshot_->value_at(position) = *cells[position];
                snapshot_->set_valid(position);
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

  private:
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
    return RowSet{std::addressof(rows), std::addressof(rows), rows.identity(),
                  [](const void *context) noexcept { return static_cast<const NativeRows *>(context)->size(); },
                  [](const void *context, row_id_t id) noexcept {
                      return static_cast<const NativeRows *>(context)->contains(id);
                  },
                  [](const void *context, std::size_t position) {
                      return static_cast<const NativeRows *>(context)->id_at(position);
                  },
                  [](const void *context, void *target, visit_function visit) {
                      const auto &source = *static_cast<const NativeRows *>(context);
                      for (auto slot : source.live_slots())
                          visit(target, source.id_at_slot(slot));
                  }};
}

inline RowSet SelectedRows::rows() const noexcept {
    return RowSet{this,
                  nullptr,
                  source_.identity,
                  [](const void *context) noexcept { return static_cast<const SelectedRows *>(context)->size(); },
                  [](const void *context, row_id_t id) noexcept {
                      const auto &rows = *static_cast<const SelectedRows *>(context);
                      return rows.contains(id) && rows.source().has(id);
                  },
                  [](const void *context, std::size_t position) {
                      const auto &rows = *static_cast<const SelectedRows *>(context);
                      if (position >= rows.size())
                          throw std::out_of_range{"selected row position"};
                      auto remaining = position;
                      auto result = no_row;
                      rows.source().each([&](row_id_t id) {
                          if (result != no_row || !rows.contains(id))
                              return;
                          if (remaining-- == 0)
                              result = id;
                      });
                      if (result == no_row)
                          throw std::out_of_range{"selected row position"};
                      return result;
                  },
                  [](const void *context, void *target, RowSet::visit_function visit) {
                      const auto &rows = *static_cast<const SelectedRows *>(context);
                      rows.source().each([&](row_id_t id) {
                          if (rows.contains(id))
                              visit(target, id);
                      });
                  }};
}

template <typename T> class NativeColumnData {
  public:
    using cell_type = std::optional<T>;
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
        return valid_cells_.contains(slot) ? cell_type{cells_[slot]} : std::nullopt;
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
    void reset_changes() noexcept { modified_.clear(); }
    void add(NativeRows &rows, row_id_t id, cell_type value, allocator memory) {
        add(rows.slots(), id, value, memory);
    }
    void add(const NativeRowSlots &rows, row_id_t id, cell_type value, allocator memory) {
        const auto slot = rows.slot_of(id);
        if (!value)
            return;
        cells_.emplace_at(slot, memory.memory_resource(), *value);
        auto constructed = false;
        auto erase_cell = util::scope_exit{[&]() noexcept {
            if (constructed)
                constructed_cells_.reset(slot);
            cells_.erase_at(slot, memory.memory_resource());
        }};
        constructed_cells_.set(slot, memory.memory_resource());
        constructed = true;
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
            cells_[slot] = *value;
        } else if (valid_cells_.contains(slot)) {
            cells_.erase_at(slot, memory.memory_resource());
            constructed_cells_.reset(slot);
            valid_cells_.reset(slot);
        }
        if (!valid_cells_.contains(slot) && value) {
            cells_.emplace_at(slot, memory.memory_resource(), *value);
            auto constructed = false;
            auto erase_cell = util::scope_exit{[&]() noexcept {
                if (constructed)
                    constructed_cells_.reset(slot);
                cells_.erase_at(slot, memory.memory_resource());
            }};
            constructed_cells_.set(slot, memory.memory_resource());
            constructed = true;
            valid_cells_.set(slot, memory.memory_resource());
            erase_cell.release();
        }
        remove_change.release();
        return true;
    }
    void erase_slot(row_id_t slot, row_id_t id, allocator memory) {
        if (valid_cells_.contains(slot)) {
            cells_.erase_at(slot, memory.memory_resource());
            constructed_cells_.reset(slot);
            valid_cells_.reset(slot);
        }
        modified_.reset(id);
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
        cells_.release(constructed_cells_, memory.memory_resource());
        constructed_cells_.release(memory.memory_resource());
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
                const auto *values =
                    cell_block.data == nullptr ? nullptr
                                               : static_cast<const T *>(cell_block.data) + first - cell_block.first;
                const auto ids = std::span<const row_id_t>{
                    static_cast<const row_id_t *>(id_block.data) + first - id_block.first, batch_end - first};
                const auto cells = ColumnBatch<T>{first, static_cast<std::size_t>(batch_end - first), values,
                                                  ColumnBatch<T>::Validity::from_index(valid_cells_)};
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
            const auto *values = cells_block.data == nullptr
                                     ? nullptr
                                     : static_cast<const T *>(cells_block.data) + first_slot - cells_block.first;
            const auto *ids = static_cast<const row_id_t *>(ids_block.data) + first_slot - ids_block.first;
            std::invoke(function,
                        Batch{std::span<const row_id_t>{ids, size},
                              ColumnBatch<T>{first_slot, size, values,
                                             ColumnBatch<T>::Validity::from_index(valid_cells_)}});
            has_batch = false;
        };

        visible_rows.each([&](row_id_t id) {
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
    util::occupancy_index constructed_cells_;
    util::sparse_bunch<T> cells_;
    util::index<> valid_cells_;
    util::index<> modified_;
};

template <typename T> class ColumnSeriesView {
  public:
    using cell_type = std::optional<T>;
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

  private:
    const NativeColumnData<T> *cells_;
    const NativeRows *rows_;
    const NativeRowSlots *physical_rows_;
    ColumnStorage<T> storage_;
    const ColumnStorage<T> *scalar_storage_;
};

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
                transitioned || !was_valid};
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

    bool set(row_id_t id, std::optional<T> value, EvaluationTime time, allocator memory) {
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
        if (has_valid_cell && batch.values == nullptr)
            throw std::invalid_argument{"computed TSC batch has valid cells but no values"};
        if (ids.empty())
            return false;

        begin_cycle(time);
        auto changed = false;
        for (std::size_t position = 0; position != ids.size(); ++position) {
            auto value = std::optional<T>{};
            if (batch.is_valid(position))
                value = batch.values[position];
            changed = cells.set(physical_rows, membership.added_rows, ids[position], value, memory) || changed;
        }
        refresh_scalar(time);
        return changed;
    }

    ColumnSeriesView<T> series_view() const noexcept { return ColumnSeriesView<T>{scalar}; }

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
    using cell_type = std::optional<T>;
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
    static constexpr bool supports_equality = false, supports_hash = false, uses_variable_memory = true;
    static constexpr bool requires_finalization = true, assign_uses_memory_resource = true;
    static value_type read(const stored_type &value) noexcept {
        return value.native != nullptr ? value_type{value} : value_type{value.snapshot};
    }
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
                        snapshot->value_at(position) = batch.values[offset];
                        snapshot->set_valid(position);
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
    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool supports_alternative_layouts = true;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = true;
    static constexpr bool assign_uses_memory_resource = true;

    static value_type read_value(const ScalarData &source) {
        const auto &type = static_cast<const frame::ColumnType &>(source.type());
        if (std::addressof(type.cell_type()) == std::addressof(cpp_scalar_type<i64>()))
            return value_type{details::scalar_data_value<frame::Column<i64>>(source)};
        if (std::addressof(type.cell_type()) == std::addressof(cpp_scalar_type<double>()))
            return value_type{details::scalar_data_value<frame::Column<double>>(source)};
        throw std::invalid_argument{"frame column scalar has an unsupported cell type"};
    }
};
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::Column<i64>>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::Column<double>>();
} // namespace bg
