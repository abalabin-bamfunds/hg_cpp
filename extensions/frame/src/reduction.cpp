#include "numeric_sum.hpp"
#include "expression_evaluation.hpp"

#include <hg/frame/tsc.hpp>
#include <hg/graph.hpp>
#include <hg/library/operators.hpp>
#include <hg/node.hpp>
#include <hg/overload.hpp>
#include <hg/util/index.hpp>
#include <hg/util/scope_exit.hpp>
#include <hg/util/sparse_bunch.hpp>

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

namespace hg::frame {
namespace {

using namespace hg::literals;

template <cell_type T> struct SumState {
    explicit SumState(allocator memory) : memory_{memory} {}
    SumState(const SumState &) = delete;
    SumState &operator=(const SumState &) = delete;

    ~SumState() { clear(); }

    void clear() noexcept {
        contributions_.release(positions_, memory_.memory_resource());
        positions_.release(memory_.memory_resource());
        sum_ = {};
    }

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

    void apply_changes(const In<TSC<T>> &input, std::pmr::memory_resource &scratch) {
        input.for_each_removed([&](row_id_t id) { erase(id); });
        if (input.has_row_id_remapping())
            input.for_each_row_id_remapping([&](row_id_t old_id, row_id_t) { erase(old_id); });
        const auto storage = input.extension_descriptor().storage;
        auto changed = details::EvaluationRows{storage.visible_rows, &scratch};
        const auto changes = std::array{input.changes()};
        changed.select_changes(changes);
        if (!changed.ids.empty()) {
            const auto columns = std::array{ColumnSeriesView<T>{storage}};
            ColumnSeriesView<T>::for_each_aligned_batch(
                std::span<const ColumnSeriesView<T>>{columns}, changed.requested, scratch,
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

        if constexpr (std::same_as<T, double>)
            recompute_finite_if_overflowed();
    }

    T result() const noexcept { return sum_.result(); }

  private:
    void update(row_id_t id, std::optional<T> value) {
        if (positions_.contains(id)) {
            if (value) {
                const auto previous = contributions_[id];
                sum_.remove(previous);
                contributions_[id] = *value;
                sum_.add(*value);
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

        sum_.remove(contributions_[id]);
        contributions_.erase_at(id, memory_.memory_resource());
        positions_.reset(id);
    }

    void insert(row_id_t id, T value) {
        positions_.set(id, memory_.memory_resource());
        auto rollback = util::scope_exit{[&]() noexcept { positions_.reset(id); }};
        contributions_.emplace_at(id, memory_.memory_resource(), value);
        rollback.release();
        sum_.add(value);
    }

    void recompute_finite_if_overflowed() {
        sum_.recompute_finite_if_overflowed([&](auto visit) {
            for (const auto id : positions_)
                visit(contributions_[id]);
        });
    }

    allocator memory_;
    util::sparse_bunch<T> contributions_;
    util::index<> positions_;
    details::NumericSum<T> sum_;
};

template <cell_type T> struct sum_tsc_node {
    In<TSC<T>> values;
    State<SumState<T> *> state;
    Out<TS<T>> output;

    void start(EvaluationContext *context) const {
        *state = context->runtime_graph().allocation().new_object<SumState<T>>(
            context->runtime_graph().allocation());
    }

    void evaluate(EvaluationContext *context) const {
        auto &cache = **state;
        if (!output.valid()) {
            cache.rebuild(values);
        } else if (values.modified()) {
            auto scratch = details::ScratchResource{context->scratch()};
            cache.apply_changes(values, scratch);
        }

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
} // namespace hg::frame
