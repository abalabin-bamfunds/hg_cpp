#include <hg/frame/aggregation.hpp>

#include "numeric_sum.hpp"

namespace hg::frame {
namespace {

template <numeric_cell_type T> class SumAggregation {
    struct Group {
        details::NumericSum<T> sum;
    };

  public:
    explicit SumAggregation(allocator memory) : memory_{memory} {}
    ~SumAggregation() {
        auto &memory = memory_.memory_resource();
        contributions_.release(valid_rows_, memory);
        valid_rows_.release(memory);
        groups_.release(group_positions_, memory);
        group_positions_.release(memory);
        recompute_groups_.release(memory);
    }

    void add_group(std::uint32_t group) {
        groups_.emplace_at(group, memory_.memory_resource());
        group_positions_.set(group, memory_.memory_resource());
    }
    void erase_group(std::uint32_t group) {
        if constexpr (std::same_as<T, double>) {
            if (recompute_groups_.contains(group)) {
                recompute_groups_.reset(group);
                --recompute_group_count_;
            }
        }
        groups_.erase_at(group, memory_.memory_resource());
        group_positions_.reset(group);
    }

    void remove(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups) {
        for (auto position = std::size_t{}; position != rows.size(); ++position)
            erase(rows[position], groups[position]);
    }

    void move(std::span<const row_id_t> rows, std::span<const std::uint32_t> previous_groups,
              std::span<const std::uint32_t> groups) {
        for (auto position = std::size_t{}; position != rows.size(); ++position) {
            const auto id = rows[position];
            if (!valid_rows_.contains(id))
                continue;
            const auto value = contributions_[id];
            groups_[previous_groups[position]].sum.remove(value);
            mark_recompute_if_overflowed(previous_groups[position]);
            groups_[groups[position]].sum.add(value);
            mark_recompute_if_overflowed(groups[position]);
        }
    }

    void update(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups,
                const ColumnBatch<T> &values) {
        for (auto position = std::size_t{}; position != rows.size(); ++position) {
            const auto id = rows[position];
            const auto group = groups[position];
            erase(id, group);
            if (!values.is_valid(position))
                continue;
            contributions_.emplace_at(id, memory_.memory_resource(), values.value_at(position));
            valid_rows_.set(id, memory_.memory_resource());
            groups_[group].sum.add(values.value_at(position));
            mark_recompute_if_overflowed(group);
        }
    }

    T result(std::uint32_t group, const AggregationRows &membership) {
        recompute_overflowed_groups(membership);
        return groups_[group].sum.result();
    }

  private:
    void mark_recompute_if_overflowed(std::uint32_t group) {
        if constexpr (std::same_as<T, double>) {
            if (groups_[group].sum.finite_overflowed() && !recompute_groups_.contains(group)) {
                recompute_groups_.set(group, memory_.memory_resource());
                ++recompute_group_count_;
            }
        }
    }

    void recompute_overflowed_groups(const AggregationRows &membership) {
        if constexpr (std::same_as<T, double>) {
            if (recompute_group_count_ == 0)
                return;

            for (const auto group : recompute_groups_)
                groups_[group].sum.reset_finite_accumulator();
            membership.rows.each([&](row_id_t id) {
                const auto group = membership.group(id);
                if (recompute_groups_.contains(group) && valid_rows_.contains(id))
                    groups_[group].sum.add_finite_contribution(contributions_[id]);
            });
            recompute_groups_.clear();
            recompute_group_count_ = 0;
        }
    }

    void erase(row_id_t id, std::uint32_t group) {
        if (!valid_rows_.contains(id))
            return;
        groups_[group].sum.remove(contributions_[id]);
        mark_recompute_if_overflowed(group);
        contributions_.erase_at(id, memory_.memory_resource());
        valid_rows_.reset(id);
    }

    allocator memory_;
    util::sparse_bunch<T> contributions_;
    util::index<> valid_rows_;
    util::sparse_bunch<Group> groups_;
    util::index<> group_positions_;
    util::index<> recompute_groups_;
    std::size_t recompute_group_count_{};
};

} // namespace

const ScalarDispatch<AggregationFunctionAdapter> &sum_aggregation_kernels() {
    static auto kernels = ScalarDispatch<AggregationFunctionAdapter>{};
    static const auto registered = [&] {
        static_cast<void>(register_aggregation_function<i64, i64, SumAggregation<i64>>(kernels));
        static_cast<void>(register_aggregation_function<double, double, SumAggregation<double>>(kernels));
        return true;
    }();
    static_cast<void>(registered);
    return kernels;
}

} // namespace hg::frame
