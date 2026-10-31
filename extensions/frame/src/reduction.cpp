#include <bg/frame/tsc.hpp>
#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/util/index.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace bg::frame {
namespace {

using namespace bg::literals;

template <cell_type T> struct SumState {
    explicit SumState(allocator memory) : memory_{memory}, changed_ids_{&memory.memory_resource()} {}
    SumState(const SumState &) = delete;
    SumState &operator=(const SumState &) = delete;

    ~SumState() { clear(); }

    void clear() noexcept {
        contributions_.release(positions_, memory_.memory_resource());
        positions_.release(memory_.memory_resource());
        total_ = T{};
        contribution_count_ = 0;
        changed_ids_.clear();
        binding_.reset();
        if constexpr (std::same_as<T, double>) {
            finite_count_ = 0;
            positive_infinity_count_ = 0;
            negative_infinity_count_ = 0;
            nan_count_ = 0;
            finite_overflow_ = false;
            finite_total_ = 0;
        }
    }

    bool same_binding(const ErasedColumnSeriesView &view) const noexcept {
        return binding_ && binding_->same_binding(view);
    }

    void remember_binding(const ErasedColumnSeriesView &view) { binding_ = view; }

    void rebuild(const In<TSC<T>> &input) {
        clear();
        input.for_each_batch([&](const auto &batch) {
            for (auto position = std::size_t{}; position != batch.cells.size; ++position) {
                if (batch.cells.is_valid(position))
                    insert(batch.ids[position], batch.cells.value_at(position));
            }
        });
        if constexpr (std::same_as<T, double>)
            recompute_finite_if_overflowed();
    }

    void apply_changes(const In<TSC<T>> &input) {
        changed_ids_.clear();
        input.for_each_changed([&](row_id_t id) { changed_ids_.push_back(id); });
        std::ranges::sort(changed_ids_);

        if (!changed_ids_.empty()) {
            const auto storage = input.extension_descriptor().storage;
            const auto rows = changed_row_set(storage.visible_rows);
            const auto columns = std::array{ColumnSeriesView<T>{storage}};
            ColumnSeriesView<T>::for_each_aligned_batch(
                std::span<const ColumnSeriesView<T>>{columns}, rows, memory_.memory_resource(),
                [&](std::span<const row_id_t> ids, std::span<const ColumnBatch<T>> batches) {
                    const auto &cells = batches.front();
                    for (auto position = std::size_t{}; position != ids.size(); ++position) {
                        if (cells.is_valid(position))
                            update(ids[position], cells.value_at(position));
                        else
                            update(ids[position], std::nullopt);
                    }
                });
        }

        input.for_each_removed([&](row_id_t id) { erase(id); });
        if constexpr (std::same_as<T, double>)
            recompute_finite_if_overflowed();
    }

    T result() const noexcept {
        if constexpr (std::same_as<T, i64>) {
            return contribution_count_ == 0 ? i64{} : total_;
        } else {
            if (contribution_count_ == 0)
                return 0;
            if (nan_count_ != 0 || (positive_infinity_count_ != 0 && negative_infinity_count_ != 0))
                return std::numeric_limits<double>::quiet_NaN();
            if (positive_infinity_count_ != 0)
                return std::numeric_limits<double>::infinity();
            if (negative_infinity_count_ != 0)
                return -std::numeric_limits<double>::infinity();
            return finite_total_;
        }
    }

  private:
    RowSet changed_row_set(const RowSet &source) const noexcept {
        return {.context = std::addressof(changed_ids_),
                .native_rows = source.native_rows,
                .identity = source.identity,
                .size = [](const void *context) noexcept {
                    return static_cast<const std::pmr::vector<row_id_t> *>(context)->size();
                },
                .contains = [](const void *context, row_id_t id) noexcept {
                    const auto &ids = *static_cast<const std::pmr::vector<row_id_t> *>(context);
                    return std::ranges::binary_search(ids, id);
                },
                .id_at = [](const void *context, std::size_t position) {
                    return (*static_cast<const std::pmr::vector<row_id_t> *>(context))[position];
                },
                .for_each = [](const void *context, void *target, RowSet::visit_function visit) {
                    for (const auto id : *static_cast<const std::pmr::vector<row_id_t> *>(context))
                        visit(target, id);
                }};
    }

    void update(row_id_t id, std::optional<T> value) {
        if (positions_.contains(id)) {
            if (value) {
                const auto previous = contributions_[id];
                remove_contribution(previous);
                contributions_[id] = *value;
                add_contribution(*value);
                return;
            }
            erase(id);
            return;
        }
        if (value)
            insert(id, *value);
    }

    void erase(row_id_t id) noexcept {
        if (!positions_.contains(id))
            return;

        remove_contribution(contributions_[id]);
        contributions_.erase_at(id, memory_.memory_resource());
        positions_.reset(id);
    }

    void insert(row_id_t id, T value) {
        positions_.set(id, memory_.memory_resource());
        auto rollback = util::scope_exit{[&]() noexcept { positions_.reset(id); }};
        contributions_.emplace_at(id, memory_.memory_resource(), value);
        rollback.release();
        add_contribution(value);
    }

    void add_contribution(T value) noexcept {
        ++contribution_count_;
        if constexpr (std::same_as<T, i64>) {
            total_ = std::bit_cast<i64>(static_cast<std::uint64_t>(total_) + static_cast<std::uint64_t>(value));
        } else {
            if (std::isnan(value)) {
                ++nan_count_;
            } else if (std::isinf(value)) {
                if (std::signbit(value))
                    ++negative_infinity_count_;
                else
                    ++positive_infinity_count_;
            } else {
                ++finite_count_;
                if (!finite_overflow_) {
                    finite_total_ += value;
                    finite_overflow_ = !std::isfinite(finite_total_);
                }
            }
        }
    }

    void remove_contribution(T value) noexcept {
        --contribution_count_;
        if constexpr (std::same_as<T, i64>) {
            total_ = std::bit_cast<i64>(static_cast<std::uint64_t>(total_) - static_cast<std::uint64_t>(value));
            if (contribution_count_ == 0)
                total_ = 0;
        } else {
            if (std::isnan(value)) {
                --nan_count_;
            } else if (std::isinf(value)) {
                if (std::signbit(value))
                    --negative_infinity_count_;
                else
                    --positive_infinity_count_;
            } else {
                --finite_count_;
                if (finite_count_ == 0) {
                    finite_total_ = 0;
                    finite_overflow_ = false;
                } else if (!finite_overflow_) {
                    finite_total_ -= value;
                    finite_overflow_ = !std::isfinite(finite_total_);
                }
            }
        }
    }

    void recompute_finite_if_overflowed() noexcept {
        if (!finite_overflow_)
            return;

        finite_total_ = 0;
        finite_overflow_ = false;
        for (const auto id : positions_) {
            const auto value = contributions_[id];
            if (!std::isfinite(value))
                continue;
            finite_total_ += value;
            if (!std::isfinite(finite_total_)) {
                finite_overflow_ = true;
                return;
            }
        }
    }

    allocator memory_;
    util::sparse_bunch<T> contributions_;
    util::index<> positions_;
    std::optional<ErasedColumnSeriesView> binding_;
    std::pmr::vector<row_id_t> changed_ids_;
    T total_{};
    std::size_t contribution_count_{};
    std::size_t finite_count_{};
    std::size_t positive_infinity_count_{};
    std::size_t negative_infinity_count_{};
    std::size_t nan_count_{};
    double finite_total_{};
    bool finite_overflow_{};
};

template <cell_type T> struct sum_tsc_node {
    In<TSC<T>> values;
    State<SumState<T> *> state;
    Out<TS<T>> output;

    void start(EvaluationContext *context) const {
        *state = context->runtime_graph().allocation().new_object<SumState<T>>(
            context->runtime_graph().allocation());
    }

    void evaluate() const {
        auto &cache = **state;
        const auto descriptor = values.extension_descriptor();
        const auto view = ErasedColumnSeriesView{descriptor.storage};
        const auto binding_changed = !cache.same_binding(view);
        const auto changes_current = values.modified() && view.change_time() == values.last_updated();
        const auto rebuild = !output.valid() || binding_changed || (values.modified() && !changes_current);
        if (rebuild) {
            cache.rebuild(values);
        } else if (changes_current) {
            cache.apply_changes(values);
        }

        cache.remember_binding(view);
        output.set(cache.result());
    }

    void on_invalidation() const noexcept {
        (*state)->clear();
        output.invalidate();
    }

    void stop(EvaluationContext *context) const noexcept {
        context->runtime_graph().allocation().delete_object(*state);
        *state = nullptr;
    }
};

struct column_reduction_overloads {
    template <cell_type T> using sum_tsc_family = sum_tsc_node<T>;
    instantiate_overloads<operators::sum_, sum_tsc_family, i64, double> tsc;
};

const auto registered = [] {
    register_overloads<column_reduction_overloads>();
    return true;
}();

} // namespace
} // namespace bg::frame
