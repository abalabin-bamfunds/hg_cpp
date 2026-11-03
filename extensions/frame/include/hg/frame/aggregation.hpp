#pragma once

#include <hg/frame/column.hpp>
#include <hg/frame/export.hpp>
#include <hg/scalar/dispatch.hpp>
#include <hg/scalar/opaque.hpp>
#include <hg/scalar/scalar.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace hg::frame {

// Extension-facing row membership. The aggregation node owns the mapping and updates it before applying kernels.
// Row IDs and group positions are independent. This view is valid only during the result call.
struct AggregationRows {
    RowSet rows;
    const void *context{};
    std::uint32_t (*group_at)(const void *, row_id_t){};

    std::uint32_t group(row_id_t id) const { return group_at(context, id); }
};

namespace details {

// Erased state consumed by the aggregation node. Registration generates its typed implementation adapter.
class AggregationState {
  public:
    virtual ~AggregationState() = default;
    virtual void add_group(std::uint32_t group) = 0;
    virtual void erase_group(std::uint32_t group) = 0;
    virtual void remove(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups) = 0;
    virtual void move(std::span<const row_id_t> rows, std::span<const std::uint32_t> previous_groups,
                      std::span<const std::uint32_t> groups) = 0;
    virtual void update(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups,
                        const ColumnBatch<> &values) = 0;
    virtual std::optional<Scalar<>> result(std::uint32_t group, const AggregationRows &membership) = 0;
};

} // namespace details

// Wiring selects this adapter from the operation's registry. Evaluation constructs one state per aggregation.
struct AggregationFunctionAdapter {
    const ScalarType *result_type{};
    std::unique_ptr<details::AggregationState> (*create)(allocator){};
};

namespace details {

template <cell_type RESULT, cell_type ARG, typename STATE>
class AggregationFunctionAdapterFactory final : public AggregationState {
    using Result = decltype(std::declval<STATE &>().result(std::uint32_t{}, std::declval<const AggregationRows &>()));
    static_assert(std::same_as<Result, cell_value_t<RESULT>> ||
                  std::same_as<Result, std::optional<cell_value_t<RESULT>>>,
                  "aggregation result must match its registered result cell type");

  public:
    explicit AggregationFunctionAdapterFactory(allocator memory) : memory_{memory}, state_{memory} {}
    ~AggregationFunctionAdapterFactory() override {
        scalar_storage_traits<RESULT>::destroy(&result_, memory_);
    }

    static std::unique_ptr<AggregationState> create(allocator memory) {
        return std::make_unique<AggregationFunctionAdapterFactory>(memory);
    }

    void add_group(std::uint32_t group) override { state_.add_group(group); }
    void erase_group(std::uint32_t group) override { state_.erase_group(group); }
    void remove(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups) override {
        state_.remove(rows, groups);
    }
    void move(std::span<const row_id_t> rows, std::span<const std::uint32_t> previous_groups,
              std::span<const std::uint32_t> groups) override {
        state_.move(rows, previous_groups, groups);
    }
    void update(std::span<const row_id_t> rows, std::span<const std::uint32_t> groups,
                const ColumnBatch<> &values) override {
        state_.update(rows, groups, values.template as<ARG>());
    }
    std::optional<Scalar<>> result(std::uint32_t group, const AggregationRows &membership) override {
        const auto value = std::optional<cell_value_t<RESULT>>{state_.result(group, membership)};
        if (!value)
            return std::nullopt;
        scalar_storage_traits<RESULT>::assign(result_, *value, memory_);
        return Scalar<>::from<RESULT>(result_);
    }

  private:
    allocator memory_;
    STATE state_;
    cell_stored_t<RESULT> result_{};
};

} // namespace details

// Extension-facing registration. STATE is constructed with allocator and owns group state and cached contributions.
// It supplies add_group, erase_group, remove, move, update(..., const ColumnBatch<ARG>&), and
// result(group, membership), returning cell_value_t<RESULT> or its optional form. Row and group spans have equal
// lengths; update values are aligned with them. Input batches are borrowed for the call. The adapter owns its
// published result storage until its next result call or destruction. Dispatch and type erasure occur once per block.
// Registration returns true when it inserts a handler. A duplicate ARG registration returns false and keeps the
// existing handler.
template <cell_type RESULT, cell_type ARG, typename STATE>
[[nodiscard]] bool register_aggregation_function(ScalarDispatch<AggregationFunctionAdapter> &functions) {
    using Factory = details::AggregationFunctionAdapterFactory<RESULT, ARG, STATE>;
    return functions.add(cpp_scalar_type<ARG>(),
                         AggregationFunctionAdapter{&cpp_scalar_type<RESULT>(), &Factory::create});
}

// An aggregation expression retains this registry as an ordinary scalar. Wiring selects its operand kernel.
using AggregationOperation = Opaque<const ScalarDispatch<AggregationFunctionAdapter> *>;

HG_FRAME_EXPORT const ScalarDispatch<AggregationFunctionAdapter> &sum_aggregation_kernels();

} // namespace hg::frame
